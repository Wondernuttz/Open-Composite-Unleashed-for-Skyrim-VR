//
// Created by ZNix on 25/10/2020.
//

#include "XrBackend.h"
#include "generated/interfaces/vrtypes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#endif

#if defined(SUPPORT_GL) && !defined(_WIN32)
#include <GL/glx.h>
#endif

#include <openxr/openxr_platform.h>

// On Android, the app has to pass the OpenGLES setup data through
#ifdef ANDROID
#include "../OpenOVR/Misc/android_api.h"
#endif

// FIXME find a better way to send the OnPostFrame call?
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseOverlay.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include "../OpenOVR/Misc/OVRPerfHook.h"
#include "generated/interfaces/IVRCompositor_018.h"


#include "tmp_gfx/TemporaryGraphics.h"

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "tmp_gfx/TemporaryD3D11.h"
#endif

#include "../OpenOVR/Misc/Config.h"
#include "ASWProvider.h"
#include "SpaceWarpProvider.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include <d3d11.h>
#endif

#include <chrono>
#include <cinttypes>
#include <mutex>
#include <ranges>
#include <type_traits>

using namespace vr;

// ── Aim pose data shared with PrismaVR via window property ──
// PrismaVR reads OC_AIM_POSES to get controller pointing direction
// without per-controller calibration constants.
// Also used by ASW to project controller positions for hand detection.
struct OCAimPoseData {
	vr::HmdMatrix34_t matrix[2]; // [0]=left, [1]=right
	bool valid[2];
};
OCAimPoseData g_aimPoses = {}; // non-static: accessed from dx11compositor for ASW hand detection
static HWND g_aimPoseHwnd = nullptr;

static BOOL CALLBACK FindGameWindowCB(HWND hwnd, LPARAM lParam)
{
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
		*reinterpret_cast<HWND*>(lParam) = hwnd;
		return FALSE;
	}
	return TRUE;
}

std::mutex inputRestartMutex;

std::unique_ptr<TemporaryGraphics> XrBackend::temporaryGraphics = nullptr;
XrBackend::XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx)
{
	memset(projectionViews, 0, sizeof(projectionViews));

	// setup temporaryGraphics

#if defined(SUPPORT_VK)
	if (useVulkanTmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryVk>();
	}
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// To prevent error code XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING with Unity games
	if (temporaryGraphics) {
		XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));
	}

	if (!temporaryGraphics && useD3D11TmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryD3D11>();
	}
#endif

	OOVR_FALSE_ABORT(temporaryGraphics);

	// setup the device indexes
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		ITrackedDevice* dev = GetDevice(i);

		if (dev)
			dev->InitialiseDevice(i);
	}
}

// --- GPU Timing via D3D11 Timestamp Queries ---
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
void XrBackend::InitGpuTiming(ID3D11Device* device)
{
	if (gpuTimingInitialized)
		return;

	D3D11_QUERY_DESC disjointDesc = {};
	disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

	D3D11_QUERY_DESC timestampDesc = {};
	timestampDesc.Query = D3D11_QUERY_TIMESTAMP;

	HRESULT hr = device->CreateQuery(&disjointDesc, &gpuTimestampDisjoint);
	if (FAILED(hr)) {
		OOVR_LOG("GPU timing: failed to create disjoint query");
		return;
	}

	hr = device->CreateQuery(&timestampDesc, &gpuTimestampBegin);
	if (FAILED(hr)) {
		OOVR_LOG("GPU timing: failed to create begin timestamp query");
		gpuTimestampDisjoint->Release();
		gpuTimestampDisjoint = nullptr;
		return;
	}

	hr = device->CreateQuery(&timestampDesc, &gpuTimestampEnd);
	if (FAILED(hr)) {
		OOVR_LOG("GPU timing: failed to create end timestamp query");
		gpuTimestampDisjoint->Release();
		gpuTimestampDisjoint = nullptr;
		gpuTimestampBegin->Release();
		gpuTimestampBegin = nullptr;
		return;
	}

	device->AddRef();
	gpuTimingDevice = device;
	device->GetImmediateContext(&gpuTimingContext);

	gpuTimingInitialized = true;
	OOVR_LOG("GPU timing: D3D11 timestamp queries initialized");
}

void XrBackend::ReadGpuTimingResults()
{
	if (!gpuTimingInFlight)
		return;

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData;
	HRESULT hr = gpuTimingContext->GetData(gpuTimestampDisjoint, &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH);

	if (hr != S_OK)
		return; // Not ready yet, keep last measured value

	if (disjointData.Disjoint) {
		gpuTimingInFlight = false;
		return; // GPU frequency changed, data unreliable
	}

	UINT64 beginTime, endTime;
	hr = gpuTimingContext->GetData(gpuTimestampBegin, &beginTime, sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr != S_OK)
		return;

	hr = gpuTimingContext->GetData(gpuTimestampEnd, &endTime, sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr != S_OK)
		return;

	float deltaMs = (float)(endTime - beginTime) / (float)disjointData.Frequency * 1000.0f;

	// Sanity check: ignore obviously wrong values
	if (deltaMs > 0.0f && deltaMs < 200.0f) {
		measuredGpuTimeMs = deltaMs;
	}

	gpuTimingInFlight = false;
}

void XrBackend::CleanupGpuTiming()
{
	if (gpuTimestampDisjoint) {
		gpuTimestampDisjoint->Release();
		gpuTimestampDisjoint = nullptr;
	}
	if (gpuTimestampBegin) {
		gpuTimestampBegin->Release();
		gpuTimestampBegin = nullptr;
	}
	if (gpuTimestampEnd) {
		gpuTimestampEnd->Release();
		gpuTimestampEnd = nullptr;
	}
	if (gpuTimingContext) {
		gpuTimingContext->Release();
		gpuTimingContext = nullptr;
	}
	if (gpuTimingDevice) {
		gpuTimingDevice->Release();
		gpuTimingDevice = nullptr;
	}
	gpuTimingInitialized = false;
	gpuTimingInFlight = false;
}
#endif

XrBackend::~XrBackend()
{
	ShutdownOVRPerfHook();

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	CleanupGpuTiming();
#endif

	// First clear out the compositors, since they might try and access the OpenXR instance
	// in their destructor.
	PrepareForSessionShutdown();

	DrvOpenXR::FullShutdown();

	graphicsBinding = nullptr;

	// This must happen after session destruction (which occurs in FullShutdown), as runtimes (namely Monado)
	// may try to access these resources while destroying the session.
	temporaryGraphics.reset();
}

XrSessionState XrBackend::GetSessionState()
{
	return sessionState;
}

IHMD* XrBackend::GetPrimaryHMD()
{
	return hmd.get();
}

ITrackedDevice* XrBackend::GetDevice(
    vr::TrackedDeviceIndex_t index)
{
	switch (index) {
	case vr::k_unTrackedDeviceIndex_Hmd:
		return GetPrimaryHMD();
	case 1:
		return hand_left.get();
	case 2:
		return hand_right.get();
	default:
		return nullptr;
	}
}

ITrackedDevice* XrBackend::GetDeviceByHand(
    ITrackedDevice::HandType hand)
{
	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return hand_left.get();
	case ITrackedDevice::HAND_RIGHT:
		return hand_right.get();
	default:
		OOVR_SOFT_ABORTF("Cannot get hand by type '%d'", (int)hand);
		return nullptr;
	}
}

void XrBackend::GetDeviceToAbsoluteTrackingPose(
    vr::ETrackingUniverseOrigin toOrigin,
    float predictedSecondsToPhotonsFromNow,
    vr::TrackedDevicePose_t* poseArray,
    uint32_t poseArrayCount)
{
	for (uint32_t i = 0; i < poseArrayCount; ++i) {
		ITrackedDevice* dev = GetDevice(i);
		if (dev) {
			dev->GetPose(toOrigin, &poseArray[i], ETrackingStateType::TrackingStateType_Rendering);
		} else {
			poseArray[i] = BackendManager::InvalidPose();
		}
	}

	// ── Controller pose caching: used by ASW for hand detection + PrismaVR ──
	// Read from poseArray (populated above for ALL games via legacy or action API).
	// Device indices: 1 = left controller, 2 = right controller.
	{
		static int s_ctrlDbg = 0;
		for (int h = 0; h < 2; h++) {
			uint32_t devIdx = (h == 0) ? 1 : 2; // left=1, right=2
			if (devIdx < poseArrayCount && poseArray[devIdx].bPoseIsValid) {
				g_aimPoses.valid[h] = true;
				g_aimPoses.matrix[h] = poseArray[devIdx].mDeviceToAbsoluteTracking;
				if (g_aswProvider) {
					auto& m = poseArray[devIdx].mDeviceToAbsoluteTracking;
					g_aswProvider->SetControllerPos(h, m.m[0][3], m.m[1][3], m.m[2][3], true);
				}
			} else {
				g_aimPoses.valid[h] = false;
				if (g_aswProvider) g_aswProvider->SetControllerPos(h, 0, 0, 0, false);
			}
		}
		if (s_ctrlDbg++ < 3) {
			OOVR_LOGF("CtrlPose: count=%u asw=%p L_valid=%d R_valid=%d L=(%.3f,%.3f,%.3f)",
			    poseArrayCount, (void*)g_aswProvider,
			    (int)g_aimPoses.valid[0], (int)g_aimPoses.valid[1],
			    g_aimPoses.matrix[0].m[0][3], g_aimPoses.matrix[0].m[1][3], g_aimPoses.matrix[0].m[2][3]);
		}
	}

	// Action-based aim poses (PrismaVR laser pointing) — only when actions are loaded.
	BaseInput* input = GetUnsafeBaseInput();
	if (input && input->AreActionsLoaded()) {
		for (int h = 0; h < 2; h++) {
			XrSpace aimSpace = XR_NULL_HANDLE;
			input->GetHandSpace((ITrackedDevice::HandType)h, aimSpace, true);
			if (aimSpace) {
				vr::TrackedDevicePose_t aimPose = {};
				xr_utils::PoseFromSpace(&aimPose, aimSpace, toOrigin);
				g_aimPoses.valid[h] = aimPose.bPoseIsValid;
				if (aimPose.bPoseIsValid)
					g_aimPoses.matrix[h] = aimPose.mDeviceToAbsoluteTracking;
			} else {
				g_aimPoses.valid[h] = false;
			}
		}

		// Expose aim pose data pointer via window property (set once)
		if (!g_aimPoseHwnd) {
			EnumWindows(FindGameWindowCB, reinterpret_cast<LPARAM>(&g_aimPoseHwnd));
			if (g_aimPoseHwnd) {
				SetPropW(g_aimPoseHwnd, L"OC_AIM_POSES", (HANDLE)&g_aimPoses);
			}
		}
	}
}

static void find_queue_family_and_queue_idx(VkDevice dev, VkPhysicalDevice pdev, VkQueue desired_queue, uint32_t& out_queueFamilyIndex, uint32_t& out_queueIndex)
{
	uint32_t queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, NULL);

	std::vector<VkQueueFamilyProperties> hi(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, hi.data());
	OOVR_LOGF("number of queue families is %d", queue_family_count);

	for (int i = 0; i < queue_family_count; i++) {
		OOVR_LOGF("queue family %d has %d queues", i, hi[i].queueCount);
		for (int j = 0; j < hi[i].queueCount; j++) {
			VkQueue tmp;
			vkGetDeviceQueue(dev, i, j, &tmp);
			if (tmp == desired_queue) {
				OOVR_LOGF("Got desired queue: %d %d", i, j);
				out_queueFamilyIndex = i;
				out_queueIndex = j;
				return;
			}
		}
	}
	OOVR_ABORT("Couldn't find the queue family index/queue index of the queue that the OpenVR app gave us!"
	           "This is really odd and really shouldn't ever happen");
}

/* Submitting Frames */
void XrBackend::CheckOrInitCompositors(const vr::Texture_t* tex)
{
	// Check we're using the session with the application's device
	if (!usingApplicationGraphicsAPI) {
		usingApplicationGraphicsAPI = true;

		OOVR_LOG("Recreating OpenXR session for application graphics API");

		// Shutdown old session - apparently Varjo doesn't like the session being destroyed
		// after querying for graphics requirements.
		DrvOpenXR::ShutdownSession();

		switch (tex->eType) {
		case vr::TextureType_DirectX: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			auto* d3dTex = (ID3D11Texture2D*)tex->handle;
			ID3D11Device* dev = nullptr;
			d3dTex->GetDevice(&dev);

			XrGraphicsBindingD3D11KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
			d3dInfo.device = dev;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

			// Initialize GPU timing queries before releasing the device
			if (oovr_global_configuration.EnableGpuTiming()) {
				// InitGpuTiming(dev); // DISABLED: D3D11 timestamp queries cause micro stutter
			}

			dev->Release();
#else
			OOVR_ABORT("Application is trying to submit a D3D11 texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		case vr::TextureType_DirectX12: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			D3D12TextureData_t* d3dTexData = (D3D12TextureData_t*)tex->handle;
			ComPtr<ID3D12Device> device;
			d3dTexData->m_pResource->GetDevice(IID_PPV_ARGS(&device));

			XrGraphicsBindingD3D12KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
			d3dInfo.device = device.Get();
			d3dInfo.queue = d3dTexData->m_pCommandQueue;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D12KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

#ifdef _DEBUG
			ComPtr<ID3D12Debug> debugController;
			D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
			debugController->EnableDebugLayer();
#endif

			device->Release();
#else
			OOVR_ABORT("Application is trying to submit a D3D12 texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		case vr::TextureType_Vulkan: {
			const vr::VRVulkanTextureData_t* vktex = (vr::VRVulkanTextureData_t*)tex->handle;

			VkPhysicalDevice xr_desire;
			// Regardless of error checking, we have to call this or we get crazy validation errors.
			xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, vktex->m_pInstance, &xr_desire);

			if (xr_desire != vktex->m_pPhysicalDevice) {
				OOVR_ABORTF("The VkPhysicalDevice that the OpenVR app (%p) used is different from the one that the OpenXR runtime used (%p)!\n"
				            "This should never happen, except for on multi-gpu, in which case DRI_PRIME=1 should fix things on Linux.",
				    vktex->m_pPhysicalDevice, xr_desire);
			}

			XrGraphicsBindingVulkanKHR binding;
			binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
			binding.next = nullptr;
			binding.instance = vktex->m_pInstance;
			binding.physicalDevice = vktex->m_pPhysicalDevice;
			binding.device = vktex->m_pDevice;

			find_queue_family_and_queue_idx( //
			    binding.device, //
			    binding.physicalDevice, //
			    vktex->m_pQueue, //
			    binding.queueFamilyIndex, //
			    binding.queueIndex //
			);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingVulkanKHR>>(binding);
			DrvOpenXR::SetupSession();
			break;
		}
		case vr::TextureType_OpenGL: {
#ifdef SUPPORT_GL
			// The spec requires that we call this before starting a session using OpenGL. Unfortunately we
			// can't actually do anything with this information, since the game has already created the context.
			XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			// Platform-specific OpenGL context stuff:
#ifdef _WIN32
			XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
			binding.hGLRC = wglGetCurrentContext();
			binding.hDC = wglGetCurrentDC();

			if (!binding.hGLRC || !binding.hDC) {
				OOVR_ABORTF("Null OpenGL GLRC or DC: %p,%p", (void*)binding.hGLRC, (void*)binding.hDC);
			}

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLWin32KHR>>(binding);
			DrvOpenXR::SetupSession();
#else
			// Only support xlib for now (same as Monado)
			// TODO wayland
			// TODO xcb

			// Unfortunately we're in a bit of a sticky situation here. We can't (as far as I can tell) get
			// the GLXFBConfig from the context or drawable, and the display might give us multiple, so
			// we can't pass it onto the runtime. If we have it we can use it to find the visual info, but
			// otherwise we can't find that either.
			//    GLXFBConfig config = some_magic_function();
			//    XVisualInfo* vi = glXGetVisualFromFBConfig(glXGetCurrentDisplay(), config);
			//    uint32_t visualid = vi->visualid;
			// So... FIXME FIXME FIXME HAAAAACK! Just pass in invalid values and hope the runtime doesn't notice!
			// Monado doesn't (and hopefully in the future, won't) use these values, so it ought to work for now.
			//
			// Note: on re-reading the spec it does appear there's no requirement that the config is the one used
			//  to create the context. That seems a bit odd so we could be technically compliant by just grabbing
			//  the first one, but it's probably better (IMO) to pass null and make the potential future issue
			//  obvious rather than wasting lots of time of the poor person who has to track it down.
			GLXFBConfig config = nullptr;
			uint32_t visualid = 0xffffffff;

			XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
			binding.xDisplay = glXGetCurrentDisplay();
			binding.visualid = visualid;
			binding.glxFBConfig = config;
			binding.glxDrawable = glXGetCurrentDrawable();
			binding.glxContext = glXGetCurrentContext();

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLXlibKHR>>(binding);
			DrvOpenXR::SetupSession();
#endif
			// End of platform-specific code

#elif defined(SUPPORT_GLES)
			// The spec requires that we call this before starting a session using OpenGL. We could actually handle this properly
			// on android since the app has to be modified to work with us, but for now don't bother.
			XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			if (!OpenComposite_Android_GLES_Binding_Info)
				OOVR_ABORT("App is trying to use GLES, but OpenComposite_Android_GLES_Binding_Info global is not set.\n"
				           "Please ensure this is set by the application.");

			XrGraphicsBindingOpenGLESAndroidKHR binding = *OpenComposite_Android_GLES_Binding_Info;
			OOVR_FALSE_ABORT(binding.type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR);
			binding.next = nullptr;

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLESAndroidKHR>>(binding);
			DrvOpenXR::SetupSession();

#else
			OOVR_ABORT("Application is trying to submit an OpenGL texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		default:
			OOVR_ABORTF("Invalid/unknown texture type %d", tex->eType);
		}

		// Real graphics binding should be setup now - get rid of temporary graphics
		temporaryGraphics.reset();
	}

	for (std::unique_ptr<Compositor>& compositor : compositors) {
		// Skip a compositor if it's already set up
		if (compositor)
			continue;

		compositor.reset(BaseCompositor::CreateCompositorAPI(tex));
	}
}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
namespace {
constexpr float kAswWaitStallThresholdMs = 30.0f;
}

void XrBackend::ResetAswSplitFrameState()
{
	aswSplitPhase = AswSplitPhase::None;
	aswWarpFrameState = XrFrameState{ XR_TYPE_FRAME_STATE };
	aswRealFrameState = XrFrameState{ XR_TYPE_FRAME_STATE };
	aswEstimatedRealDisplayTime = 0;
}

bool XrBackend::ShouldUseAswSplitPipeline() const
{
	// Split-frame pipeline is only for custom PC-side ASW, not Meta space warp.
	// When g_spaceWarpProvider is active, the runtime handles reprojection —
	// no warp frames needed from our side.
	if (g_spaceWarpProvider && g_spaceWarpProvider->IsReady())
		return false;

	return g_aswProvider && g_aswProvider->IsReady() && g_aswProvider->HasPreviousCachedFrame()
	    && !g_aswProvider->IsPaused()
	    && oovr_global_configuration.ASWEnabled() && sessionActive
	    && aswStallCount < 5;
}

void XrBackend::EndActiveFrameEmpty(XrTime displayTime)
{
	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.displayTime = displayTime;
	endInfo.layers = nullptr;
	endInfo.layerCount = 0;
	xrEndFrame(xr_session.get(), &endInfo);
}

void XrBackend::LatchViewsForDisplayTime(XrTime displayTime)
{
	xr_gbl->nextPredictedFrameTime = displayTime;

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = displayTime;
	locateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	OOVR_FAILED_XR_SOFT_ABORT(xrLocateViews(xr_session.get(), &locateInfo, &viewState, XruEyeCount, &viewCount, views));

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;

		XrPosef pose = views[eye].pose;
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		}
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
			pose.position = XrVector3f{ 0, 1.75, 0 };
		}

		projectionViews[eye].pose = pose;
	}

	xr_gbl->latchedViews[0] = views[0];
	xr_gbl->latchedViews[1] = views[1];
	xr_gbl->latchedViewStateFlags = viewState.viewStateFlags;
	xr_gbl->viewSpaceViewsLatched = false;
	xr_gbl->viewsLatched = true;
	{
		static bool s = false;
		if (!s) {
			s = true;
			OOVR_LOGF("[diag] WaitForTrackingData: views LATCHED (viewsLatched=true)");
#ifdef _WIN32
			DWORD jiggleVal = 0;
			DWORD jiggleSize = sizeof(jiggleVal);
			LONG jiggleResult = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Virtual Desktop, Inc.\\OpenXR",
			    L"jiggle_view_rotations", RRF_RT_REG_DWORD, nullptr, &jiggleVal, &jiggleSize);
			if (jiggleResult == ERROR_SUCCESS)
				OOVR_LOGF("[diag] VDXR registry: jiggle_view_rotations = %lu", jiggleVal);
			else if (jiggleResult == ERROR_FILE_NOT_FOUND)
				OOVR_LOGF("[diag] VDXR registry: jiggle_view_rotations NOT SET (key absent)");
			else
				OOVR_LOGF("[diag] VDXR registry: jiggle_view_rotations read failed (error %ld)", jiggleResult);
#endif
		}
	}
}

bool XrBackend::SubmitAswWarpFrame(const XrFrameState& frameState,
    XrCompositionLayerBaseHeader const* const* extraLayers,
    int extraLayerCount)
{
	if (!g_aswProvider || !g_aswProvider->IsReady()) {
		EndActiveFrameEmpty(frameState.predictedDisplayTime);
		return false;
	}

	ID3D11DeviceContext* aswCtx = nullptr;
	if (g_aswProvider->GetDevice())
		g_aswProvider->GetDevice()->GetImmediateContext(&aswCtx);
	if (!aswCtx) {
		EndActiveFrameEmpty(frameState.predictedDisplayTime);
		return false;
	}

	bool submittedWarp = false;

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = frameState.predictedDisplayTime;
	locateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	xrLocateViews(xr_session.get(), &locateInfo, &viewState, XruEyeCount, &viewCount, views);

	// Read thumbstick state at warp time. When sticks are idle, zero out the loco
	// direction signal so forward scatter uses the correct fill direction, and zero
	// yaw injection so poseDeltaMatrix doesn't include stale stick rotation.
	// The game MV residual (totalMV - c2cHeadMV) naturally handles loco deceleration.
	{
		static constexpr float kStickDead = 0.10f;
		auto readStick = [](XrAction action) -> float {
			if (action == XR_NULL_HANDLE || xr_session.get() == XR_NULL_HANDLE) return 0.0f;
			XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
			info.action = action;
			XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
			if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session.get(), &info, &state)) && state.isActive)
				return state.currentState;
			return 0.0f;
		};

		float leftX = readStick(xr_leftStickX_action);
		float leftY = readStick(xr_leftStickY_action);
		float rightX = readStick(xr_rightStickX_action);

		bool locoStickIdle = (leftX * leftX + leftY * leftY) < (kStickDead * kStickDead);
		bool rotStickIdle = fabsf(rightX) < kStickDead;

		if (locoStickIdle)
			g_aswProvider->SetLocomotionTranslation(0.0f, 0.0f, 0.0f);
		if (rotStickIdle)
			g_aswProvider->SetLocomotionYaw(0.0f);

		// Independent stop handling for locomotion and rotation.
		// Each has its own frame counter and confidence suppression so they
		// can stop independently without affecting the other.
		static int s_locoStopFrames = 0;
		static bool s_locoWasActive = false;
		static int s_rotStopFrames = 0;
		static bool s_rotWasActive = false;

		// Locomotion stop → suppress mvConfidenceScale (affects loco+animation residual)
		if (!locoStickIdle) {
			s_locoWasActive = true;
			s_locoStopFrames = 0;
			g_aswProvider->SetMVConfidenceScale(1.0f);
		} else if (s_locoWasActive && s_locoStopFrames < 3) {
			float scale = (s_locoStopFrames == 0) ? 0.33f : 0.0f;
			g_aswProvider->SetMVConfidenceScale(scale);
			s_locoStopFrames++;
		} else {
			g_aswProvider->SetMVConfidenceScale(1.0f);
			s_locoWasActive = false;
		}

		// Rotation stop → suppress rotMVScale (affects only rotation component)
		if (!rotStickIdle) {
			s_rotWasActive = true;
			s_rotStopFrames = 0;
			g_aswProvider->SetRotMVScale(1.0f);
		} else if (s_rotWasActive && s_rotStopFrames < 3) {
			g_aswProvider->SetRotMVScale(0.0f);
			s_rotStopFrames++;
		} else {
			g_aswProvider->SetRotMVScale(1.0f);
			s_rotWasActive = false;
		}
	}

	// On stop transition, use the most recent cache slot (N-0) instead of N-1.
	// Combined with zero MV confidence, this reprojects the last game frame
	// with head-tracking only — minimal discontinuity with the next game frame.
	bool stopping = (g_aswProvider->GetMVConfidenceScale() < 0.5f)
	             || (g_aswProvider->GetRotMVScale() < 0.5f);
	int slotOverride = stopping ? g_aswProvider->GetPublishedSlot() : -1;

	// Fetch FRESH controller positions at warp time using the same coordinate path
	// as CacheFrame (GetDeviceToAbsoluteTrackingPose → g_aimPoses → SetControllerPos).
	// This ensures the warp-time positions are in the same space as the cached UVs.
	{
		vr::TrackedDevicePose_t warpPoses[3] = {};
		GetDeviceToAbsoluteTrackingPose(
		    vr::TrackingUniverseStanding, 0.0f, warpPoses, 3);
		// Device 1 = left, 2 = right (same as CacheFrame path)
		for (int h = 0; h < 2; h++) {
			uint32_t devIdx = (h == 0) ? 1 : 2;
			if (warpPoses[devIdx].bPoseIsValid) {
				auto& m = warpPoses[devIdx].mDeviceToAbsoluteTracking;
				g_aswProvider->SetControllerPos(h, m.m[0][3], m.m[1][3], m.m[2][3], true);
			}
		}
	}

	bool warpOk = true;
	for (int eye = 0; eye < 2; eye++) {
		if (!g_aswProvider->WarpFrame(eye, views[eye].pose, slotOverride)) {
			warpOk = false;
			break;
		}
	}

	if (warpOk && g_aswProvider->SubmitWarpedOutput(aswCtx)) {
		XrCompositionLayerProjectionView warpedViews[2] = {};
		XrCompositionLayerDepthInfoKHR depthInfo[2] = {};
		bool hasDepth = (g_aswProvider->GetDepthSwapchain() != XR_NULL_HANDLE);

		for (int eye = 0; eye < 2; eye++) {
			warpedViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			warpedViews[eye].pose = g_aswProvider->GetPrecompPose(eye);
			warpedViews[eye].fov = g_aswProvider->GetCachedFov(eye);
			warpedViews[eye].subImage.swapchain = g_aswProvider->GetOutputSwapchain();
			warpedViews[eye].subImage.imageArrayIndex = 0;
			warpedViews[eye].subImage.imageRect = g_aswProvider->GetOutputRect(eye);

			if (hasDepth) {
				depthInfo[eye].type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
				depthInfo[eye].next = nullptr;
				depthInfo[eye].subImage.swapchain = g_aswProvider->GetDepthSwapchain();
				depthInfo[eye].subImage.imageArrayIndex = 0;
				depthInfo[eye].subImage.imageRect = g_aswProvider->GetOutputRect(eye);
				depthInfo[eye].minDepth = 0.0f;
				depthInfo[eye].maxDepth = 1.0f;
				depthInfo[eye].nearZ = g_aswProvider->GetCachedNear();
				depthInfo[eye].farZ = g_aswProvider->GetCachedFar();
				warpedViews[eye].next = &depthInfo[eye];
			}
		}

		XrCompositionLayerProjection warpedLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		warpedLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		warpedLayer.views = warpedViews;
		warpedLayer.viewCount = 2;

		std::vector<XrCompositionLayerBaseHeader const*> aswLayers;
		aswLayers.push_back((XrCompositionLayerBaseHeader*)&warpedLayer);
		for (int i = 1; i < extraLayerCount; i++)
			aswLayers.push_back(extraLayers[i]);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.layers = aswLayers.data();
		endInfo.layerCount = (uint32_t)aswLayers.size();
		xrEndFrame(xr_session.get(), &endInfo);
		submittedWarp = true;
	} else {
		EndActiveFrameEmpty(frameState.predictedDisplayTime);
	}

	aswCtx->Release();
	return submittedWarp;
}

bool XrBackend::BeginAswWarpFrameForSplit()
{
	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	aswWarpFrameState = XrFrameState{ XR_TYPE_FRAME_STATE };

	QueryPerformanceCounter(&waitFrameStart);
	XrResult waitRes = xrWaitFrame(xr_session.get(), &waitInfo, &aswWarpFrameState);
	QueryPerformanceCounter(&waitFrameEnd);
	measuredWaitFrameMs = (float)(waitFrameEnd.QuadPart - waitFrameStart.QuadPart) * 1000.0f / (float)qpcFrequency.QuadPart;

	if (XR_FAILED(waitRes)) {
		OOVR_LOGF("ASW split: xrWaitFrame(warp) failed result=%d", (int)waitRes);
		return false;
	}

	if (aswWarpFrameState.predictedDisplayPeriod > 0)
		predictedDisplayPeriodMs = (float)(aswWarpFrameState.predictedDisplayPeriod / 1000000.0);

	if (measuredWaitFrameMs > kAswWaitStallThresholdMs) {
		aswStallCount++;
		OOVR_LOGF("ASW split: xrWaitFrame(warp) stalled %.1fms (stall %d/5)",
		    measuredWaitFrameMs, aswStallCount);
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(xr_session.get(), &beginInfo);
		EndActiveFrameEmpty(aswWarpFrameState.predictedDisplayTime);
		return false;
	}

	aswStallCount = 0;

	XrDuration realOffset = aswWarpFrameState.predictedDisplayPeriod;
	if (realOffset <= 0 && predictedDisplayPeriodMs > 0.0f)
		realOffset = (XrDuration)(predictedDisplayPeriodMs * 1000000.0f);
	if (realOffset <= 0) {
		OOVR_LOG("ASW split: runtime gave no predicted display period; falling back to inline warp");
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(xr_session.get(), &beginInfo);
		EndActiveFrameEmpty(aswWarpFrameState.predictedDisplayTime);
		return false;
	}

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));

	aswEstimatedRealDisplayTime = aswWarpFrameState.predictedDisplayTime + realOffset;
	LatchViewsForDisplayTime(aswEstimatedRealDisplayTime);
	aswSplitPhase = AswSplitPhase::WarpFrameBegun;

	return true;
}

bool XrBackend::BeginRealFrameAfterAswWarp(float* outWaitMs)
{
	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	aswRealFrameState = XrFrameState{ XR_TYPE_FRAME_STATE };
	auto t0 = std::chrono::high_resolution_clock::now();
	XrResult waitRes = xrWaitFrame(xr_session.get(), &waitInfo, &aswRealFrameState);
	auto t1 = std::chrono::high_resolution_clock::now();
	float realWaitMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
	if (outWaitMs)
		*outWaitMs = realWaitMs;

	if (XR_FAILED(waitRes)) {
		OOVR_LOGF("ASW split: xrWaitFrame(real) failed result=%d", (int)waitRes);
		renderingFrame = false;
		submittedEyeTextures = false;
		ResetAswSplitFrameState();
		return false;
	}

	if (aswRealFrameState.predictedDisplayPeriod > 0)
		predictedDisplayPeriodMs = (float)(aswRealFrameState.predictedDisplayPeriod / 1000000.0);

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));
	QueryPerformanceCounter(&beginFrameQpc);

	aswSplitPhase = AswSplitPhase::RealFrameBegun;
	return true;
}

void XrBackend::FinishAswWarpFrameAfterFirstEye(float cpuToFirstSubmitMs, float firstSubmitInvokeMs)
{
	if (aswSplitPhase != AswSplitPhase::WarpFrameBegun)
		return;

	(void)cpuToFirstSubmitMs;
	(void)firstSubmitInvokeMs;

	SubmitAswWarpFrame(aswWarpFrameState, nullptr, 0);
	BeginRealFrameAfterAswWarp(nullptr);
}

#endif

void XrBackend::WaitForTrackingData()
{
	// Make sure the OpenXR session is active before doing anything else, and if not then skip
	if (!sessionActive) {
		renderingFrame = false;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		ResetAswSplitFrameState();
#endif
		return;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };

	// Initialize QPC frequency once
	if (!qpcInitialized) {
		QueryPerformanceFrequency(&qpcFrequency);
		qpcInitialized = true;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	ResetAswSplitFrameState();
#endif

	{
		auto lock = xr_session.lock_shared();

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		if (ShouldUseAswSplitPipeline() && BeginAswWarpFrameForSplit())
			goto wait_done;
#endif

		QueryPerformanceCounter(&waitFrameStart);
		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		QueryPerformanceCounter(&waitFrameEnd);
		measuredWaitFrameMs = (float)(waitFrameEnd.QuadPart - waitFrameStart.QuadPart) * 1000.0f / (float)qpcFrequency.QuadPart;
		if (measuredWaitFrameMs > 1000.0f) {
			OOVR_LOGF("ASW FREEZE: xrWaitFrame blocked %.1fms — possible TDR or runtime stall", measuredWaitFrameMs);
		}

		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

		// Store the runtime's actual display period (nanoseconds → milliseconds)
		if (state.predictedDisplayPeriod > 0) {
			predictedDisplayPeriodMs = (float)(state.predictedDisplayPeriod / 1000000.0);
		}

		// xrBeginFrame stays adjacent to xrWaitFrame for consistent frame pacing.
		// Deferring it introduced variable latency from xrLocateViews that caused
		// VDXR's prediction model to see inconsistent timing → periodic micro-stutters.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));
		QueryPerformanceCounter(&beginFrameQpc);
	}

	LatchViewsForDisplayTime(xr_gbl->nextPredictedFrameTime);
wait_done:

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// GPU timing: read previous frame's results, then start new measurement
	if (gpuTimingInitialized) {
		ReadGpuTimingResults();
		gpuTimingContext->Begin(gpuTimestampDisjoint);
		gpuTimingContext->End(gpuTimestampBegin);
		gpuTimingInFlight = true;
	}
#endif

	// If we're not on the game's graphics API yet, don't actually mark us as having started the frame.
	// Instead, set a different flag so we'll call this method again when it's available.
	if (!usingApplicationGraphicsAPI) {
		deferredRenderingStart = true;
	} else {
		renderingFrame = true;
	}

	// Mark CPU frame start — game gets control back now
	QueryPerformanceCounter(&cpuFrameStart);
}

void XrBackend::StoreEyeTexture(
    vr::EVREye eye,
    const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags,
    bool isFirstEye)
{
	CheckOrInitCompositors(texture);

	XrCompositionLayerProjectionView& layer = projectionViews[eye];
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

	std::unique_ptr<Compositor>& compPtr = compositors[eye];
	OOVR_FALSE_ABORT(compPtr.get() != nullptr);
	Compositor& comp = *compPtr;

	bool eyeStored = false;
	float submitInvokeMs = 0.0f;

	// If the session is inactive, we may be unable to write to the surface
	if (sessionActive && renderingFrame) {
		auto invokeStart = std::chrono::high_resolution_clock::now();
		comp.Invoke((XruEye)eye, texture, bounds, submitFlags, layer);
		auto invokeEnd = std::chrono::high_resolution_clock::now();
		submitInvokeMs = std::chrono::duration<float, std::milli>(invokeEnd - invokeStart).count();
		eyeStored = true;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (eyeStored && isFirstEye && aswSplitPhase == AswSplitPhase::WarpFrameBegun) {
		float cpuToFirstSubmitMs = 0.0f;
		if (qpcInitialized && cpuFrameStart.QuadPart > 0) {
			LARGE_INTEGER firstSubmitQpc = {};
			QueryPerformanceCounter(&firstSubmitQpc);
			cpuToFirstSubmitMs = (float)(firstSubmitQpc.QuadPart - cpuFrameStart.QuadPart)
			    * 1000.0f / (float)qpcFrequency.QuadPart;
		}
		FinishAswWarpFrameAfterFirstEye(cpuToFirstSubmitMs, submitInvokeMs);
	}
#endif

	if (eyeStored && renderingFrame)
		submittedEyeTextures = true;

	// TODO store view somewhere and use it for submitting our frame

	// If WaitGetPoses was called before the first texture was submitted, we're in a kinda weird state
	// The application will expect it can submit it's frames (and we do too) however xrBeginFrame was
	// never called for this session - it was called for the early session, then when the first texture
	// was published we switched to that, but this new session hasn't had xrBeginFrame called yet.
	// To get around this, we set a flag if we should begin a frame but are still in the early session. At
	// this point we can check for that flag and call xrBeginFrame a second time, on the right session.
	if (deferredRenderingStart && usingApplicationGraphicsAPI) {
		deferredRenderingStart = false;
		WaitForTrackingData();
	}
}

void XrBackend::SubmitFrames(bool showSkybox, bool postPresent)
{
	static std::mutex submitMutex;
	std::lock_guard<std::mutex> lock(submitMutex);

	// Always pump events, even if the session isn't active - this is what makes the session active
	// in the first place.
	PumpEvents();

	// If we are getting calls from PostPresentHandOff then skip the calls from other functions as
	//  there will be other data such as GUI layers to be added before ending the frame.
	bool skipRender = postPresentStatus && !postPresent;
	postPresentStatus = postPresent;

	if (!renderingFrame || skipRender)
		return;

	// All data submitted, rendering has finished, frame can be ended.
	renderingFrame = false;

	// Make sure the OpenXR session is active before doing anything else
	// Note that if the session becomes ready after WaitGetTrackingPoses was called, then
	// renderingFrame will still be false so this won't be a problem in that case.
	if (!sessionActive) {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		ResetAswSplitFrameState();
#endif
		return;
	}

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = xr_gbl->nextPredictedFrameTime;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (aswSplitPhase == AswSplitPhase::RealFrameBegun && aswRealFrameState.predictedDisplayTime != 0)
		info.displayTime = aswRealFrameState.predictedDisplayTime;
#endif

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	XrCompositionLayerBaseHeader* app_layer = nullptr;

	int layer_count = 0;

	// Apps can use layers to provide GUIs and loading screens where a 3D environment is not being rendered.
	// Only create the projection layer if we have a 3D environment to submit.
	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (submittedEyeTextures) {
		// We have eye textures so setup a projection layer
		mainLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;

		app_layer = (XrCompositionLayerBaseHeader*)&mainLayer;
		for (int i = 0; i < mainLayer.viewCount; ++i) {
			XrCompositionLayerProjectionView& layer = projectionViews[i];
			layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			if (layer.subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		// Chain XR_FB_space_warp info to each projection view when Meta space warp is active.
		// The SpaceWarpProvider's info structs were filled during SubmitFrame() in dx11compositor.
		if (g_spaceWarpProvider && g_spaceWarpProvider->IsReady() && app_layer) {
			for (int i = 0; i < 2; i++) {
				auto* swInfo = g_spaceWarpProvider->GetLayerInfo(i);
				// Chain space warp after any existing next (e.g. depth info set by compositor)
				swInfo->next = projectionViews[i].next;
				projectionViews[i].next = swInfo;
			}
			static int s_log = 0;
			if (s_log++ < 5)
				OOVR_LOG("SpaceWarp: Chained layer info to projection views");
		}
#endif

		submittedEyeTextures = false;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// GPU timing: decide whether to extend measurement through _BuildLayers.
	// VRKeyboard constructor creates XR swapchains and D3D11 resources —
	// doing that inside an active timestamp disjoint query can crash some
	// OpenXR runtimes. So we only keep the query active through _BuildLayers
	// when the keyboard already exists (no construction will happen).
	// On the rare frame where keyboard is first created, we end timing early.
	bool gpuTimingExtendedThroughOverlay = false;
	if (gpuTimingInitialized && gpuTimingInFlight) {
		BaseOverlay* preCheckOverlay = GetUnsafeBaseOverlay();
		if (preCheckOverlay && preCheckOverlay->IsKeyboardActive()) {
			// Keyboard exists — safe to let timing run through _BuildLayers
			gpuTimingExtendedThroughOverlay = true;
		} else {
			// No keyboard yet (might be created) — end timing now for safety
			gpuTimingContext->End(gpuTimestampEnd);
			gpuTimingContext->End(gpuTimestampDisjoint);
		}
	}
#endif

	// Ensure the BaseOverlay singleton exists so the keyboard shortcut
	// detection in _BuildLayers runs every frame. Without this, the overlay
	// is only created when the game explicitly requests IVROverlay, which
	// some games do late (or not at all until a cell transition).
	static std::shared_ptr<BaseOverlay> overlayHolder;
	if (!GetUnsafeBaseOverlay()) {
		overlayHolder = GetCreateBaseOverlay();
	}

	// If we have an overlay then add
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (overlay) {
		layer_count = overlay->_BuildLayers(app_layer, headers);
	} else if (app_layer) {
		layer_count = 1;
		headers = &app_layer;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// End GPU timing after _BuildLayers (if we extended through it)
	if (gpuTimingExtendedThroughOverlay && gpuTimingInFlight) {
		gpuTimingContext->End(gpuTimestampEnd);
		gpuTimingContext->End(gpuTimestampDisjoint);
	}
#endif

	// It's ok if no layers have been added at this point,
	// it will just cause the display to be blanked
	info.layers = headers;
	info.layerCount = layer_count;

	// CPU frame time: game work is done, measure before xrEndFrame
	if (qpcInitialized && cpuFrameStart.QuadPart > 0) {
		QueryPerformanceCounter(&cpuFrameEnd);
		measuredCpuFrameMs = (float)(cpuFrameEnd.QuadPart - cpuFrameStart.QuadPart) * 1000.0f / (float)qpcFrequency.QuadPart;
	}

	// Compositor time: measure xrEndFrame duration
	QueryPerformanceCounter(&endFrameStart);
	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));
	QueryPerformanceCounter(&endFrameEnd);
	if (qpcInitialized) {
		measuredEndFrameMs = (float)(endFrameEnd.QuadPart - endFrameStart.QuadPart) * 1000.0f / (float)qpcFrequency.QuadPart;

		// Frame-to-frame interval
		if (lastFrameSubmitQpc.QuadPart > 0) {
			measuredFrameIntervalMs = (float)(endFrameEnd.QuadPart - lastFrameSubmitQpc.QuadPart) * 1000.0f / (float)qpcFrequency.QuadPart;
		}
		lastFrameSubmitQpc = endFrameEnd;

		// Compositor overhead: residual = frame_interval - app_gpu - app_cpu - xrEndFrame_cpu
		// This estimates the time the runtime spends on reprojection/distortion on the GPU.
		// Clamped to [0, displayPeriod] to prevent garbage from measurement error.
		float frameInterval = predictedDisplayPeriodMs > 0.0f ? predictedDisplayPeriodMs : measuredFrameIntervalMs;
		if (frameInterval > 0.0f) {
			float appGpu = 0.0f;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
			appGpu = (gpuTimingInitialized && measuredGpuTimeMs > 0.0f) ? measuredGpuTimeMs : 0.0f;
#endif
			float residual = frameInterval - measuredCpuFrameMs - appGpu - measuredEndFrameMs;
			float maxClamp = frameInterval;
			compositorOverheadMs = (residual < 0.0f) ? 0.0f : (residual > maxClamp ? maxClamp : residual);
		}
	}

	// ── OCU ASW: Hot-reload settings from ini (1-second file watcher) ──
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	{
		static ULONGLONG lastAswCheck = 0;
		static FILETIME lastAswWriteTime = {};
		ULONGLONG now = GetTickCount64();
		if (now - lastAswCheck > 1000) {
			lastAswCheck = now;
			wchar_t exePath[MAX_PATH];
			GetModuleFileNameW(nullptr, exePath, MAX_PATH);
			std::wstring iniPath(exePath);
			size_t sp = iniPath.find_last_of(L"\\/");
			if (sp != std::wstring::npos)
				iniPath = iniPath.substr(0, sp + 1);
			iniPath += L"opencomposite.ini";
			WIN32_FILE_ATTRIBUTE_DATA fad = {};
			if (GetFileAttributesExW(iniPath.c_str(), GetFileExInfoStandard, &fad)) {
				if (CompareFileTime(&fad.ftLastWriteTime, &lastAswWriteTime) != 0) {
					lastAswWriteTime = fad.ftLastWriteTime;
					FILE* f = _wfopen(iniPath.c_str(), L"r");
					if (f) {
						char line[512];
						bool inDefaultSection = true;
						while (fgets(line, sizeof(line), f)) {
							if (line[0] == '[') {
								inDefaultSection = false;
								continue;
							}
							if (!inDefaultSection)
								continue;
							float fval;
							if (sscanf(line, "aswWarpStrength=%f", &fval) == 1)
								oovr_global_configuration.aswWarpStrength = fval;
							else if (sscanf(line, "aswRotationScale=%f", &fval) == 1)
								oovr_global_configuration.aswRotationScale = fval;
							else if (sscanf(line, "aswTranslationScale=%f", &fval) == 1)
								oovr_global_configuration.aswTranslationScale = fval;
							else if (sscanf(line, "aswDepthScale=%f", &fval) == 1)
								oovr_global_configuration.aswDepthScale = fval;
						}
						fclose(f);
					}
				}
			}
		}
	}
#endif

	// ── OCU ASW: Inject warped frame ──
	// After the real frame is submitted, claim the next display slot and submit
	// a warped version of the cached frame. This doubles the effective framerate
	// sent to the VR runtime, eliminating the need for SSW.
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	#if 0
	static int s_aswStallCount = 0; // consecutive frames where xrWaitFrame took too long
	if (g_aswProvider && g_aswProvider->IsReady() && g_aswProvider->HasCachedFrame()
	    && oovr_global_configuration.ASWEnabled() && sessionActive
	    && s_aswStallCount < 5) { // disable after 5 consecutive stalls

		// Get D3D11 context from ASWProvider's device (independent of GPU timing)
		ID3D11DeviceContext* aswCtx = nullptr;
		if (g_aswProvider->GetDevice()) {
			g_aswProvider->GetDevice()->GetImmediateContext(&aswCtx);
		}
		if (aswCtx) {
			auto lock = xr_session.lock_shared();

			// 1. Claim next display slot (measure time — xrWaitFrame can block the game)
			auto t0 = std::chrono::high_resolution_clock::now();
			XrFrameWaitInfo aswWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
			XrFrameState aswState{ XR_TYPE_FRAME_STATE };
			XrResult res = xrWaitFrame(xr_session.get(), &aswWaitInfo, &aswState);
			auto t1 = std::chrono::high_resolution_clock::now();
			float waitMs = std::chrono::duration<float, std::milli>(t1 - t0).count();

			if (XR_SUCCEEDED(res)) {
				// Check if xrWaitFrame took unreasonably long (>30ms = missed a whole frame)
				if (waitMs > 30.0f) {
					s_aswStallCount++;
					OOVR_LOGF("ASW: xrWaitFrame stalled %.1fms (stall %d/5)", waitMs, s_aswStallCount);
					// Submit empty frame to keep runtime in sync, then skip
					XrFrameBeginInfo aswBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
					xrBeginFrame(xr_session.get(), &aswBeginInfo);
					XrFrameEndInfo aswEndInfo{ XR_TYPE_FRAME_END_INFO };
					aswEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
					aswEndInfo.displayTime = aswState.predictedDisplayTime;
					aswEndInfo.layers = nullptr;
					aswEndInfo.layerCount = 0;
					xrEndFrame(xr_session.get(), &aswEndInfo);
					aswCtx->Release();
					goto asw_done;
				}
				s_aswStallCount = 0; // reset on successful fast wait

				// 2. Begin frame
				XrFrameBeginInfo aswBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
				res = xrBeginFrame(xr_session.get(), &aswBeginInfo);

				if (XR_SUCCEEDED(res)) {
					// 3. Get new head pose at the new predicted display time
					XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
					locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					locateInfo.displayTime = aswState.predictedDisplayTime;
					locateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
					XrViewState viewState = { XR_TYPE_VIEW_STATE };
					uint32_t viewCount = 0;
					XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
					xrLocateViews(xr_session.get(), &locateInfo, &viewState, XruEyeCount, &viewCount, views);

					// 4. Warp cached frame — translation/parallax only (rotation=0, handled by runtime ATW)
					bool warpOk = true;
					for (int eye = 0; eye < 2; eye++) {
						if (!g_aswProvider->WarpFrame(eye, views[eye].pose)) {
							warpOk = false;
							break;
						}
					}

					// 5. Submit warped frame to XR swapchain
					if (warpOk && g_aswProvider->SubmitWarpedOutput(aswCtx)) {
						// 6. Build projection layer — use CACHED pose so runtime ATW corrects to current
						XrCompositionLayerProjectionView warpedViews[2] = {};

						// Attach depth info if depth swapchain is available
						XrCompositionLayerDepthInfoKHR depthInfo[2] = {};
						bool hasDepth = (g_aswProvider->GetDepthSwapchain() != XR_NULL_HANDLE);

						for (int eye = 0; eye < 2; eye++) {
							warpedViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
							warpedViews[eye].pose = g_aswProvider->GetPrecompPose(eye);
							warpedViews[eye].fov = g_aswProvider->GetCachedFov(eye);
							warpedViews[eye].subImage.swapchain = g_aswProvider->GetOutputSwapchain();
							warpedViews[eye].subImage.imageArrayIndex = 0;
							warpedViews[eye].subImage.imageRect = g_aswProvider->GetOutputRect(eye);

							if (hasDepth) {
								depthInfo[eye].type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
								depthInfo[eye].next = nullptr;
								depthInfo[eye].subImage.swapchain = g_aswProvider->GetDepthSwapchain();
								depthInfo[eye].subImage.imageArrayIndex = 0;
								depthInfo[eye].subImage.imageRect = g_aswProvider->GetOutputRect(eye);
								depthInfo[eye].minDepth = 0.0f;
								depthInfo[eye].maxDepth = 1.0f;
								depthInfo[eye].nearZ = g_aswProvider->GetCachedNear();
								depthInfo[eye].farZ = g_aswProvider->GetCachedFar();
								warpedViews[eye].next = &depthInfo[eye];
							}
						}

						XrCompositionLayerProjection warpedLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
						warpedLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
						warpedLayer.views = warpedViews;
						warpedLayer.viewCount = 2;

						// Build ASW layer array: warped projection + overlay layers from real frame
						// (overlay layers = keyboard quad, laser beams, etc. at index 1+ of headers)
						std::vector<XrCompositionLayerBaseHeader const*> aswLayers;
						aswLayers.push_back((XrCompositionLayerBaseHeader*)&warpedLayer);
						for (int i = 1; i < layer_count; i++) {
							aswLayers.push_back(headers[i]);
						}

						XrFrameEndInfo aswEndInfo{ XR_TYPE_FRAME_END_INFO };
						aswEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
						aswEndInfo.displayTime = aswState.predictedDisplayTime;
						aswEndInfo.layers = aswLayers.data();
						aswEndInfo.layerCount = (uint32_t)aswLayers.size();

						XrResult endRes = xrEndFrame(xr_session.get(), &aswEndInfo);
						{
							static int s = 0;
							if (s++ < 5)
								OOVR_LOGF("ASW: Warped frame injected (result=%d)", (int)endRes);
						}
					} else {
						// Warp failed — submit empty frame to keep runtime in sync
						XrFrameEndInfo aswEndInfo{ XR_TYPE_FRAME_END_INFO };
						aswEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
						aswEndInfo.displayTime = aswState.predictedDisplayTime;
						aswEndInfo.layers = nullptr;
						aswEndInfo.layerCount = 0;
						xrEndFrame(xr_session.get(), &aswEndInfo);
					}
				}
			} else {
				static int s = 0;
				if (s++ < 3)
					OOVR_LOGF("ASW: xrWaitFrame for warped slot failed result=%d", (int)res);
			}
			aswCtx->Release(); // GetImmediateContext adds a ref
		}
	} else if (s_aswStallCount >= 5 && g_aswProvider && oovr_global_configuration.ASWEnabled()) {
		static bool s_warned = false;
		if (!s_warned) {
			OOVR_LOG("ASW: Disabled — xrWaitFrame stalled 5 consecutive frames. Restart game to re-enable.");
			s_warned = true;
		}
	}
asw_done:
	#endif
	if (aswStallCount >= 5 && g_aswProvider && oovr_global_configuration.ASWEnabled()) {
		if (!aswDisableWarned) {
			OOVR_LOG("ASW: Disabled - xrWaitFrame stalled 5 consecutive frames. Restart game to re-enable.");
			aswDisableWarned = true;
		}
	}
#endif

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		sys->_OnPostFrame();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;

	nFrameIndex++;

	// Release pose latch so next frame gets fresh xrLocateViews data
	xr_gbl->viewsLatched = false;
	xr_gbl->viewSpaceViewsLatched = false;
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	ResetAswSplitFrameState();
#endif
}

IBackend::openvr_enum_t XrBackend::SetSkyboxOverride(const vr::Texture_t* pTextures, uint32_t unTextureCount)
{
	// Needed for rFactor2 loading screens
	if (unTextureCount && pTextures) {
		CheckOrInitCompositors(pTextures);

		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;

		// Make sure any unfinished frames don't call xrEndFrame after this call
		renderingFrame = false;

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };

		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

		// This submits a frame when a skybox override is set. This is designed around rFactor2 where the skybox is used as
		// a loading screen and is frequently updated, and most other games probably behave in a similar manner. It'd be
		// ideal to run a separate thread while the skybox override is set to submit frames if IVRCompositor->Submit is not
		// being called frequently enough, and that'd need to be carefully synchronised with the main submit thread. That's
		// not yet implemented since it's not currently worth the hassle, but if someone in the future wants to do it:
		// TODO submit skybox frames in their own thread.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));

		static std::unique_ptr<Compositor> compositor = nullptr;

		if (compositor == nullptr)
			compositor.reset(BaseCompositor::CreateCompositorAPI(pTextures));

		vr::VRTextureBounds_t bounds;
		bounds.uMin = 0.0;
		bounds.uMax = 1.0;
		bounds.vMin = 1.0;
		bounds.vMax = 0.0;

		compositor->Invoke(pTextures, &bounds);
		XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		layerQuad.next = NULL;
		layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
			{ 0.0f, 0.0f, -0.65f } };
		layerQuad.size = { 1.0f, 1.0f / 1.333f };
		layerQuad.subImage = {
			compositor->GetSwapChain(),
			{ { 0, 0 },
			    { (int32_t)compositor->GetSrcSize().width,
			        (int32_t)compositor->GetSrcSize().height } },
			0
		};

		XrCompositionLayerBaseHeader* layers[1];
		layers[0] = (XrCompositionLayerBaseHeader*)&layerQuad;
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		info.displayTime = xr_gbl->nextPredictedFrameTime;
		info.layers = layers;
		info.layerCount = 1;

		OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));

	} else {
		OOVR_SOFT_ABORT("Unsupported texture count");
	}

	return 0;
}

void XrBackend::ClearSkyboxOverride()
{
	OOVR_SOFT_ABORT("No implementation");
}

/* Misc compositor */

/**
 * Get frame timing information to be passed to the application
 *
 * Returns true if successful
 */
bool XrBackend::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	// Zero everything except the size field
	memset(reinterpret_cast<unsigned char*>(pTiming) + sizeof(pTiming->m_nSize), 0, pTiming->m_nSize - sizeof(pTiming->m_nSize));

	if (pTiming->m_nSize >= sizeof(IVRCompositor_018::Compositor_FrameTiming)) {
		pTiming->m_flSystemTimeInSeconds = frameSubmitTimeUs;
		pTiming->m_nFrameIndex = nFrameIndex;

		pTiming->m_nNumFramePresents = 1;
		pTiming->m_nNumMisPresented = 0;

		// --- OVR perf hook: real compositor data when available ---
		OVRPerfData ovrPerf = GetOVRPerfData();

		if (ovrPerf.available && ovrPerf.aswActive) {
			pTiming->m_nReprojectionFlags = VRCompositor_ReprojectionAsync;
		} else {
			pTiming->m_nReprojectionFlags = 0;
		}

		pTiming->m_nNumDroppedFrames = ovrPerf.available
		    ? (ovrPerf.appDroppedFrames + ovrPerf.compositorDroppedFrames)
		    : 0;

		// --- GPU timing ---
		float displayPeriod = predictedDisplayPeriodMs > 0.0f ? predictedDisplayPeriodMs : 11.1f;

		if (ovrPerf.available && ovrPerf.appGpuMs > 0.0f) {
			// Real app GPU time from OVR compositor
			pTiming->m_flPreSubmitGpuMs = ovrPerf.appGpuMs;
			pTiming->m_flPostSubmitGpuMs = measuredEndFrameMs;
			pTiming->m_flTotalRenderGpuMs = ovrPerf.appGpuMs + measuredEndFrameMs;
		} else
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
		    if (gpuTimingInitialized && measuredGpuTimeMs > 0.0f) {
			pTiming->m_flPreSubmitGpuMs = measuredGpuTimeMs;
			pTiming->m_flPostSubmitGpuMs = measuredEndFrameMs;
			pTiming->m_flTotalRenderGpuMs = measuredGpuTimeMs + measuredEndFrameMs;
		} else
#endif
		{
			pTiming->m_flPreSubmitGpuMs = displayPeriod * 0.7f;
			pTiming->m_flPostSubmitGpuMs = displayPeriod * 0.1f;
			pTiming->m_flTotalRenderGpuMs = displayPeriod * 0.8f;
		}

		// --- Compositor timing ---
		if (ovrPerf.available && ovrPerf.compositorGpuMs > 0.0f) {
			// Real compositor timing from OVR hook
			pTiming->m_flCompositorRenderGpuMs = ovrPerf.compositorGpuMs;
			pTiming->m_flCompositorRenderCpuMs = ovrPerf.compositorCpuMs;
		} else {
			// Fallback: residual estimate
			pTiming->m_flCompositorRenderGpuMs = compositorOverheadMs > 0.0f ? compositorOverheadMs : displayPeriod * 0.1f;
			pTiming->m_flCompositorRenderCpuMs = measuredEndFrameMs > 0.0f ? measuredEndFrameMs : displayPeriod * 0.05f;
		}

		pTiming->m_flCompositorIdleCpuMs = measuredWaitFrameMs > 0.0f ? measuredWaitFrameMs : 0.1f;

		// --- Measured intervals ---
		// Frame interval: prefer measured, fall back to runtime's display period
		pTiming->m_flClientFrameIntervalMs = measuredFrameIntervalMs > 0.0f ? measuredFrameIntervalMs : displayPeriod;
		pTiming->m_flPresentCallCpuMs = measuredEndFrameMs;
		pTiming->m_flWaitForPresentCpuMs = measuredWaitFrameMs;
		pTiming->m_flSubmitFrameMs = measuredCpuFrameMs > 0.0f ? measuredCpuFrameMs : 0.0f;

		// --- Relative timestamps (ms offsets from frame reference point) ---
		// Reference point: waitFrameStart (beginning of the frame cycle).
		// Convert QPC deltas to milliseconds relative to that reference.
		float qpcToMs = (qpcInitialized && qpcFrequency.QuadPart > 0)
		    ? (1000.0f / (float)qpcFrequency.QuadPart)
		    : 0.0f;

		if (qpcToMs > 0.0f && waitFrameStart.QuadPart > 0) {
			// WaitGetPoses was called at the frame reference point (offset = 0)
			pTiming->m_flWaitGetPosesCalledMs = 0.0f;

			// Poses became ready when xrWaitFrame returned
			pTiming->m_flNewPosesReadyMs = (float)(waitFrameEnd.QuadPart - waitFrameStart.QuadPart) * qpcToMs;

			// New frame ready = when Submit/xrEndFrame completed
			if (endFrameEnd.QuadPart > 0) {
				pTiming->m_flNewFrameReadyMs = (float)(endFrameEnd.QuadPart - waitFrameStart.QuadPart) * qpcToMs;
			}

			// Compositor update start = when xrEndFrame was called
			if (endFrameStart.QuadPart > 0) {
				pTiming->m_flCompositorUpdateStartMs = (float)(endFrameStart.QuadPart - waitFrameStart.QuadPart) * qpcToMs;
			}

			// Compositor update end = when xrEndFrame returned
			if (endFrameEnd.QuadPart > 0) {
				pTiming->m_flCompositorUpdateEndMs = (float)(endFrameEnd.QuadPart - waitFrameStart.QuadPart) * qpcToMs;
			}

			// Compositor render start = when xrBeginFrame was called (deferred)
			if (beginFrameQpc.QuadPart > 0) {
				pTiming->m_flCompositorRenderStartMs = (float)(beginFrameQpc.QuadPart - waitFrameStart.QuadPart) * qpcToMs;
			}
		}

		GetPrimaryHMD()->GetPose(vr::ETrackingUniverseOrigin::TrackingUniverseSeated, &pTiming->m_HmdPose, ETrackingStateType::TrackingStateType_Rendering);

		return true;
	}

	return false;
}

/* D3D Mirror textures */
/* #if defined(SUPPORT_DX) */
IBackend::openvr_enum_t XrBackend::GetMirrorTextureD3D11(vr::EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
	return 0;
}
void XrBackend::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
}
/* #endif */
/** Returns the points of the Play Area. */
bool XrBackend::GetPlayAreaPoints(vr::HmdVector3_t* points, int* count)
{
	if (count)
		*count = 0;

	XrExtent2Df bounds;
	XrResult res = xrGetReferenceSpaceBoundsRect(xr_session.get(), XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

	if (res == XR_SPACE_BOUNDS_UNAVAILABLE)
		return false;

	OOVR_FAILED_XR_ABORT(res);

	if (count)
		*count = 4;

	// The origin of the free space is centred around the player
	// TODO if we're using the Oculus runtime, grab it's native handle and get the full polygon
	if (points) {
		points[0] = vr::HmdVector3_t{ -bounds.width / 2, 0, -bounds.height / 2 };
		points[1] = vr::HmdVector3_t{ bounds.width / 2, 0, -bounds.height / 2 };
		points[2] = vr::HmdVector3_t{ bounds.width / 2, 0, bounds.height / 2 };
		points[3] = vr::HmdVector3_t{ -bounds.width / 2, 0, bounds.height / 2 };
	}

	return true;
}
/** Determine whether the bounds are showing right now **/
bool XrBackend::AreBoundsVisible()
{
	OOVR_SOFT_ABORT("No implementation");
	return false;
}
/** Set the boundaries to be visible or not (although setting this to false shouldn't affect
 * what happens if the player moves their hands too close and shows it that way) **/
void XrBackend::ForceBoundsVisible(bool status)
{
	OOVR_SOFT_ABORT("No implementation");
}

bool XrBackend::IsInputAvailable()
{
	return sessionState == XR_SESSION_STATE_FOCUSED;
}

void XrBackend::PumpEvents()
{
	BaseInput* input = GetUnsafeBaseInput();
	if (input && sessionState == XR_SESSION_STATE_FOCUSED && !hand_left && !hand_right) {
		if (input->AreActionsLoaded()) {
			UpdateInteractionProfile();
		}
	}
	// Poll for OpenXR events
	// TODO filter by session?
	while (true) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrResult res;
		OOVR_FAILED_XR_ABORT(res = xrPollEvent(xr_instance, &ev));

		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* changed = (XrEventDataSessionStateChanged*)&ev;
			OOVR_FALSE_ABORT(changed->session == xr_session.get());
			sessionState = changed->state;

			// Monado bug: it returns 0 for this value (at least for the first two states)
			// Make sure this is actually greater than 0, otherwise this will mess up xr_gbl->GetBestTime()
			if (changed->time > 0 && xr_gbl)
				xr_gbl->latestTime = changed->time;

			OOVR_LOGF("Switch to OpenXR state %d", sessionState);

			switch (sessionState) {
			case XR_SESSION_STATE_READY: {
				OOVR_LOG("Hit ready state, begin session...");
				// Start the session running - this means we're supposed to start submitting frames
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				OOVR_FAILED_XR_ABORT(xrBeginSession(xr_session.get(), &beginInfo));
				sessionActive = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				// End the session. The session is still valid and we can still query some information
				// from it, but we're not allowed to submit frames anymore. This is done when the engagement
				// sensor detects the user has taken off the headset, for example.
				OOVR_FAILED_XR_ABORT(xrEndSession(xr_session.get()));
				sessionActive = false;
				renderingFrame = false;
				break;
			}
			case XR_SESSION_STATE_EXITING: {
				OOVR_LOGF("Exiting");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING: {
				// If the headset is unplugged or the user decides to exit the app
				// TODO just kill the app after awhile, unless it sends a message to stop that - read the OpenVR wiki docs for more info
				VREvent_t quit = { VREvent_Quit };
				auto system = GetBaseSystem();
				if (system)
					system->_EnqueueEvent(quit);
				break;
			}
			default:
				// suppress clion warning about missing branches
				break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			UpdateInteractionProfile();
			break;
		}

	} // while loop

	/*
	   We check for AreActionsLoaded here because:
	   1. Games using legacy input call xrSyncActions every frame anyway, so the runtime should
	      give us an interaction profile without us forcing it
	   2. Games using an action manifest should be calling UpdateActionState every frame, which calls xrSyncActions.
	      This means the only games we wouldn't be able to confidently grab an interaction profile from
	      would be ones where an action manifest is loaded but UpdateActionState is not being called
	      (because the game checks IsTrackedDeviceConnected or something),
	      and hopefully no game like that exists.
	   Some runtimes (WMR) do not instantly return an interaction profile,
	   so we will keep tryinig to query it until it does.

	   Note that we check that the session is focused because this means that the application
	   has already submitted a frame, that frame is visible, and we have input focus.
	   Waiting until the application has input focus allows us to avoid unnecessarily restarting the
	   session when we can't even receive input anyway, as well as before the session is restarted for
	   the temporary session.
   */
}

void XrBackend::OnSessionCreated()
{
	sessionState = XR_SESSION_STATE_UNKNOWN;
	sessionActive = false;
	renderingFrame = false;

	PumpEvents();

	// Wait until we transition to the idle state.
	// This sets the time, so OpenXR calls which use that will work correctly.
	while (sessionState == XR_SESSION_STATE_UNKNOWN) {
		const int durationMs = 250;

		OOVR_LOGF("No session transition yet received, waiting %dms ...", durationMs);

#ifdef _WIN32
		Sleep(durationMs);
#else
		struct timespec ts = { 0, durationMs * 1000000 };
		nanosleep(&ts, &ts);
#endif

		PumpEvents();
	}

	// OVR perf hook disabled: MinHook + mutex per-frame overhead causes micro stutter.
	// if (InitOVRPerfHook()) {
	// 	OOVR_LOG("OVR compositor timing hook active — real perf data available");
	// }
}

void XrBackend::PrepareForSessionShutdown()
{
	for (std::unique_ptr<Compositor>& c : compositors) {
		c.reset();
	}
	if (infoSet != XR_NULL_HANDLE) {
		OOVR_FAILED_XR_ABORT(xrDestroyActionSet(infoSet));
		infoSet = XR_NULL_HANDLE;
		infoAction = XR_NULL_HANDLE;
	}
}

// On Android, add an event poll function for use while sleeping
#ifdef ANDROID
void OpenComposite_Android_EventPoll()
{
	BackendManager::Instance().PumpEvents();
}
#endif

bool XrBackend::IsGraphicsConfigured()
{
	return usingApplicationGraphicsAPI;
}

void XrBackend::OnOverlayTexture(const vr::Texture_t* texture)
{
	if (!usingApplicationGraphicsAPI)
		CheckOrInitCompositors(texture);
}

void XrBackend::UpdateInteractionProfile()
{
	struct hand_info {
		const char* pathstr;
		std::unique_ptr<XrController>& controller;
		const XrController::XrControllerType hand;
	};

	hand_info hands[] = {
		{ .pathstr = "/user/hand/left", .controller = hand_left, .hand = XrController::XCT_LEFT },
		{ .pathstr = "/user/hand/right", .controller = hand_right, .hand = XrController::XCT_RIGHT }
	};

	for (hand_info& info : hands) {
		XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
		XrPath path;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, info.pathstr, &path));
		OOVR_FAILED_XR_ABORT(xrGetCurrentInteractionProfile(xr_session.get(), path, &state));

		// interaction profile detected
		if (state.interactionProfile != XR_NULL_PATH) {
			uint32_t tmp;
			char path_name[XR_MAX_PATH_LENGTH];
			OOVR_FAILED_XR_ABORT(xrPathToString(xr_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &tmp, path_name));

			for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
				if (profile->GetPath() == path_name) {
					OOVR_LOGF("%s - Using interaction profile: %s", info.pathstr, path_name);
					info.controller = std::make_unique<XrController>(info.hand, *profile);
					hmd->SetInteractionProfile(profile.get());
					BaseSystem* system = GetUnsafeBaseSystem();
					if (system) {
						VREvent_t event = {
							.eventType = VREvent_TrackedDeviceActivated,
							.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
						};
						system->_EnqueueEvent(event);
						event = {
							.eventType = VREvent_TrackedDeviceUpdated,
							.trackedDeviceIndex = 0
						};
						system->_EnqueueEvent(event);
					}
					break;
				}
			}
			if (!hand_left && !hand_right) {
				// Runtime returned an unknown interaction profile!
				OOVR_ABORTF("Runtiime unexpectedly returned an unknown interaction profile: %s", path_name);
			}
		} else {
			// interaction profile lost/not detected
			OOVR_LOGF("%s - No interaction profile detected", info.pathstr);
			if (info.controller) {
				info.controller.reset();
				BaseSystem* system = GetUnsafeBaseSystem();
				if (system) {
					VREvent_t event = {
						.eventType = VREvent_TrackedDeviceDeactivated,
						.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
					};
					system->_EnqueueEvent(event);
				}
			}
		}
	}
}

void XrBackend::MaybeRestartForInputs()
{
	// if we haven't attached any actions to the session (infoSet or game actions), no need to restart
	BaseInput* input = GetUnsafeBaseInput();
	// if (infoSet == XR_NULL_HANDLE && (!input || !input->AreActionsLoaded()))
	// return;

	OOVR_LOG("Restarting session for inputs...");
	DrvOpenXR::SetupSession();
	OOVR_LOG("Session restart successful!");
}

void XrBackend::QueryForInteractionProfile()
{
	// Note that we want to avoid using BaseInput here because it would allow for games to call GetControllerState before rendering
	// and then we'd have to recreate the session twice, once for the input state and once for when the game submits a frame
	if (subactionPaths[0] == XR_NULL_PATH) {
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/left", &subactionPaths[0]));
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/right", &subactionPaths[1]));
	}

	if (infoSet == XR_NULL_HANDLE) {
		OOVR_LOG("Creating infoset");
		CreateInfoSet();
		BindInfoSet();
	}

	// Interaction profiles are updated after xrSyncActions, so we'll try to make the runtime give us one by calling xrSyncActions.
	XrActiveActionSet active[2] = {
		{ .actionSet = infoSet, .subactionPath = subactionPaths[0] },
		{ .actionSet = infoSet, .subactionPath = subactionPaths[1] },
	};

	XrActionsSyncInfo info{ XR_TYPE_ACTIONS_SYNC_INFO };
	info.countActiveActionSets = 2;
	info.activeActionSets = active;

	OOVR_FAILED_XR_ABORT(xrSyncActions(xr_session.get(), &info));
}

void XrBackend::CreateInfoSet()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "opencomposite-internal-info-set");
	strcpy_s(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenComposite internal info set");
	OOVR_FAILED_XR_ABORT(xrCreateActionSet(xr_instance, &set_info, &infoSet));

	XrActionCreateInfo act_info{ XR_TYPE_ACTION_CREATE_INFO };
	strcpy_s(act_info.actionName, XR_MAX_ACTION_NAME_SIZE, "opencomposite-internal-info-act");
	strcpy_s(act_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "OpenComposite internal info action");
	act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	act_info.countSubactionPaths = std::size(subactionPaths);
	act_info.subactionPaths = subactionPaths;
	OOVR_FAILED_XR_ABORT(xrCreateAction(infoSet, &act_info, &infoAction));
}

void XrBackend::BindInfoSet()
{
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		XrPath interactionProfilePath;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));
		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		std::vector<XrActionSuggestedBinding> bindings;
		// grabs the first found paths ending in /click for each subaction path
		for (const std::string& path_name : { "/user/hand/left", "/user/hand/right" }) {
			auto click_path = std::ranges::find_if(profile->GetValidInputPaths(),
			    [&path_name](std::string s) -> bool {
				    return s.find("/click") != s.npos && s.find(path_name) != s.npos;
			    });
			if (click_path == profile->GetValidInputPaths().end()) {
				continue;
			}
			XrPath path;
			OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, click_path->c_str(), &path));
			bindings.push_back({ .action = infoAction, .binding = path });

			suggestedBindings.interactionProfile = interactionProfilePath;
			suggestedBindings.suggestedBindings = bindings.data();
			suggestedBindings.countSuggestedBindings = bindings.size();

			OOVR_FAILED_XR_ABORT(xrSuggestInteractionProfileBindings(xr_instance, &suggestedBindings));
		}
	}

	// Attach the info set by itself. We will have to restart the session once the game attaches its real inputs.
	XrSessionActionSetsAttachInfo info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	info.countActionSets = 1;
	info.actionSets = &infoSet;
	OOVR_FAILED_XR_ABORT(xrAttachSessionActionSets(xr_session.get(), &info));
}

const void* XrBackend::GetCurrentGraphicsBinding()
{
	if (graphicsBinding) {
		return graphicsBinding->asVoid();
	}
	OOVR_FALSE_ABORT(temporaryGraphics);
	return temporaryGraphics->GetGraphicsBinding();
}

#ifdef SUPPORT_VK
void XrBackend::VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out)
{
	*out = VK_NULL_HANDLE;

	TemporaryVk* vk = temporaryGraphics->GetAsVk();
	if (vk == nullptr)
		OOVR_ABORT("Not using temporary Vulkan instance");

	// Find the UUID of the physical device the temporary instance is running on
	VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
	vkGetPhysicalDeviceProperties2(vk->physicalDevice, &props);

	// Look through all the physical devices on the target instance and find the matching one
	uint32_t devCount;
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, nullptr));
	std::vector<VkPhysicalDevice> physicalDevices(devCount);
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, physicalDevices.data()));

	for (VkPhysicalDevice phy : physicalDevices) {
		VkPhysicalDeviceIDProperties devIdProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 devProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &devIdProps };
		vkGetPhysicalDeviceProperties2(phy, &devProps);

		if (memcmp(devIdProps.deviceUUID, idProps.deviceUUID, sizeof(devIdProps.deviceUUID)) != 0)
			continue;

		// Found it
		*out = phy;
		return;
	}

	OOVR_ABORT("Could not find matching Vulkan physical device for instance");
}

#endif
