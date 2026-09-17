//
// Created by ZNix on 25/10/2020.
//

#include "DrvOpenXR.h"

#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/android_api.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "XrBackend.h"
#include "generated/static_bases.gen.h"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>

static XrBackend* currentBackend;
static bool initialised = false;
static std::shared_ptr<BaseInput> sessionInputKeepalive;
static uint32_t viveTrackerInteractionVersion = 0;

uint32_t DrvOpenXR::GetViveTrackerInteractionVersion()
{
	return viveTrackerInteractionVersion;
}

#ifdef _WIN32
static std::string DiagnosticUtf8(const wchar_t* value)
{
	const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
	if (length <= 0) return "<unavailable>";
	std::string result(static_cast<size_t>(length), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), length, nullptr, nullptr);
	result.pop_back();
	return result;
}

static std::string DiagnosticModulePath(HMODULE module)
{
	wchar_t path[4096]{};
	const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
	if (!length || length >= std::size(path)) return "<unavailable-or-truncated>";
	return DiagnosticUtf8(path);
}

static std::string DiagnosticEnvironmentPath(const wchar_t* name)
{
	wchar_t value[4096]{};
	const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
	if (!length) return "<unset-or-empty>";
	if (length >= std::size(value)) return "<truncated>";
	return DiagnosticUtf8(value);
}

// Capture process identity from inside Skyrim, where MO2's virtual filesystem
// and environment apply. An external collector may resolve different DLL files.
static void LogRuntimeProcessIdentity()
{
	SYSTEMTIME utc{};
	GetSystemTime(&utc);
	HMODULE module = nullptr;
	const bool foundModule = GetModuleHandleExW(
	    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    reinterpret_cast<LPCWSTR>(&LogRuntimeProcessIdentity), &module) != FALSE;
	const auto executable = DiagnosticModulePath(nullptr);
	const auto ownModule = foundModule ? DiagnosticModulePath(module) : "<unavailable>";
	OOVR_LOGF("OCU process identity: pid=%lu utc=%04u-%02u-%02uT%02u:%02u:%02u.%03uZ executable=%s module=%s",
	    GetCurrentProcessId(), utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
	    utc.wSecond, utc.wMilliseconds, executable.c_str(), ownModule.c_str());
	wchar_t activeRuntime[4096]{};
	DWORD bytes = sizeof(activeRuntime);
	const auto registryResult = RegGetValueW(HKEY_LOCAL_MACHINE,
	    L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
	    RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, activeRuntime, &bytes);
	const auto configuredRuntime = registryResult == ERROR_SUCCESS
	    ? DiagnosticUtf8(activeRuntime) : "<unavailable>";
	const auto runtimeOverride = DiagnosticEnvironmentPath(L"XR_RUNTIME_JSON");
	const auto openVRPath = DiagnosticEnvironmentPath(L"VR_OVERRIDE");
	OOVR_LOGF("OCU runtime selection: registryActiveRuntime=%s registryResult=%ld XR_RUNTIME_JSON=%s VR_OVERRIDE=%s (configured paths; actual OpenXR runtime identity follows)",
	    configuredRuntime.c_str(), registryResult, runtimeOverride.c_str(), openVRPath.c_str());
}

// xrEnumerateApiLayerProperties only reports EXPLICIT layers. The layers that
// silently wrap every xr* call — the Vive/Oculus/WMR runtime compat shims that
// cause teardown crashes and phantom device identity — are IMPLICIT and invisible
// to the app. Read them straight from the OpenXR loader's registry so they land in
// our log for support. (yujujo, 2026-07-05: five foreign implicit layers on an Index.)
static void LogImplicitOpenXRLayersFromHive(HKEY root, const char* rootName)
{
	HKEY key;
	if (RegOpenKeyExA(root, "SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit", 0, KEY_READ, &key) != ERROR_SUCCESS)
		return;

	char valueName[1024];
	for (DWORD idx = 0;; ++idx) {
		DWORD nameLen = sizeof(valueName);
		DWORD type = 0, data = 0, dataLen = sizeof(data);
		LSTATUS st = RegEnumValueA(key, idx, valueName, &nameLen, nullptr, &type, (LPBYTE)&data, &dataLen);
		if (st != ERROR_SUCCESS)
			break; // ERROR_NO_MORE_ITEMS or failure — done

		// Loader convention (matches Vulkan): DWORD 0 = enabled, non-zero = disabled.
		bool enabled = (type == REG_DWORD) ? (data == 0) : true;

		std::string lower = valueName;
		for (char& c : lower)
			if (c >= 'A' && c <= 'Z')
				c += 32;
		bool foreign = lower.find("oculus") != std::string::npos || lower.find("meta") != std::string::npos
		    || lower.find("libovr") != std::string::npos || lower.find("vive") != std::string::npos
		    || lower.find("htc") != std::string::npos || lower.find("mixedreality") != std::string::npos;

		OOVR_LOGF("Implicit OpenXR layer [%s]: %s (%s%s)", rootName, valueName,
		    enabled ? "ENABLED" : "disabled",
		    (enabled && foreign) ? " -- FOREIGN RUNTIME, possible teardown/tracking conflict" : "");
	}
	RegCloseKey(key);
}

static void LogImplicitOpenXRLayers()
{
	OOVR_LOG("Enumerating IMPLICIT OpenXR API layers from registry (invisible to xrEnumerateApiLayerProperties):");
	LogImplicitOpenXRLayersFromHive(HKEY_LOCAL_MACHINE, "HKLM");
	LogImplicitOpenXRLayersFromHive(HKEY_CURRENT_USER, "HKCU");
}
#else
static void LogRuntimeProcessIdentity() {}
static void LogImplicitOpenXRLayers() {}
#endif

#ifdef _WIN32
std::string GetExeName()
{
	char exePath[MAX_PATH + 1] = { 0 };
	DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
	PathStripPathA(exePath);
	return { exePath };
}
#else
#include <libgen.h> // basename
#include <linux/limits.h> // PATH_MAX
#include <unistd.h> // readlink

std::string GetExeName()
{
	char exePath[PATH_MAX + 1] = { 0 };
	ssize_t count = readlink("/proc/self/exe", exePath, PATH_MAX);
	if (count != -1) {
		return { basename(exePath) };
	}
	return { "" };
}
#endif

void DrvOpenXR::GetXRAppName(char (&appName)[128])
{
	std::string exeName = GetExeName();
	if (exeName.size() > 0) {
		std::string ocAppName{ "OpenComposite_" };
		ocAppName += exeName;
		// Strip .exe so VD can match game profiles but Steam won't auto-launch SteamVR
		auto pos = ocAppName.find(".exe");
		if (pos != std::string::npos && pos > 0)
			ocAppName = ocAppName.substr(0, pos);
		OOVR_LOGF("Setting application name to %s", ocAppName.c_str());
		strcpy_arr(appName, ocAppName.c_str());
	} else {
		strcpy_arr(appName, "OpenComposite");
	}
}

#ifdef _DEBUG
static XrDebugUtilsMessengerEXT dbgMessenger = NULL;

static XrBool32 XRAPI_CALL debugCallback(
    XrDebugUtilsMessageSeverityFlagsEXT messageSeverity,
    XrDebugUtilsMessageTypeFlagsEXT messageTypes,
    const XrDebugUtilsMessengerCallbackDataEXT* callbackData,
    void* userData)
{
	OOVR_LOGF("debugCallback %s", callbackData->message);
	return XR_FALSE;
}
#endif

static void CreateSystemID()
{
	// Create a system - this is when we choose what form factor we want, in this case an HMD
	XrSystemGetInfo systemInfo{};
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	OOVR_FAILED_XR_ABORT(xrGetSystem(xr_instance, &systemInfo, &xr_system));
}

IBackend* DrvOpenXR::CreateOpenXRBackend()
{
	OOVR_LOG("OCU runtime build: 4.3.11-rdm-color-coverage1 / dapa-mask-lease-v1 / stage-recenter-v1 + controller-calibration-v1 / external-locomotion-v1 + cached-head-v1 + bridge-publish-v1 + startup-checkpoints-v1 / body-tracker-validation-v2 + live-pose-status-v1 / eye-shape-offset-v1 + ring-visual-masks-v2 + scene-blackout-cull-v1 + dapa-blackout-guard-v1 / rdm-thread-hooks2-perf4 + rdm-color-coverage-v1 + rdm-depth-binding-batch-v1 + rdm-mask-reuse-v1 + guide-state-cache-v2 + rdm-work-sampling-v1 / rdm-reject-diag-v2 / rdm-sampled-diagnostics-v1 + immutable-guide-zero-v1 / compute-state-restore-v1 + depth-read-hazard-v1 / fixed-ring-no-timeout-v1 / foveation-geometry-v3-fixed-separate + rdm-depth-scope-v3 / eye-presets-v3-performance-1x1-2x2-4x2 + gaze-upload-v3 / DAPA menu-pause-v1 / DAPA mask-frame-v1 + exact-mask-v1 / terrain-depth-guard-v1 / cutout-material-guard-v1 / ring-debug-v2-quads / runtime-route-v2 / first-stereo-frame-v1 / controller-index-v1 / moving-gaze-v2 + effect-foveation-v1 / Index-grip-touch-v1 / input-recovery-v5 / DAPA render-permission-v4 + GPU timing v1");
	LogRuntimeProcessIdentity();
	// TODO handle something like Unity which stops and restarts the instance
	if (initialised) {
		OOVR_ABORT("Cannot double-initialise OpenXR");
	}
	initialised = true;

	// TODO make these work on Linux
#ifdef XR_VALIDATION_LAYER_PATH
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_CORE_VALIDATION_EXPORT_TYPE", "text"));
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_API_LAYER_PATH", XR_VALIDATION_LAYER_PATH));
	OOVR_LOGF("Set OpenXR Layer path: %s", XR_VALIDATION_LAYER_PATH);
#endif
#ifdef XR_VALIDATION_FILE_NAME
	OOVR_FALSE_ABORT(SetEnvironmentVariableA("XR_CORE_VALIDATION_FILE_NAME", XR_VALIDATION_FILE_NAME));
	OOVR_LOGF("Set OpenXR validation file path: %s", XR_VALIDATION_FILE_NAME);
#endif

	// Enumerate the available extensions
	uint32_t availableExtensionsCount;
	OOVR_FAILED_XR_ABORT(xrEnumerateInstanceExtensionProperties(nullptr, 0, &availableExtensionsCount, nullptr));
	std::vector<XrExtensionProperties> extensionProperties;
	extensionProperties.resize(availableExtensionsCount, { XR_TYPE_EXTENSION_PROPERTIES });
	OOVR_FAILED_XR_ABORT(xrEnumerateInstanceExtensionProperties(nullptr,
	    extensionProperties.size(), &availableExtensionsCount, extensionProperties.data()));
	std::set<std::string> availableExtensions;
	viveTrackerInteractionVersion = 0;
	xr_htcxViveTrackers = false;
	for (const XrExtensionProperties& ext : extensionProperties) {
		availableExtensions.insert(ext.extensionName);
		if (strcmp(ext.extensionName, "XR_HTCX_vive_tracker_interaction") == 0)
			viveTrackerInteractionVersion = ext.extensionVersion;
		OOVR_LOGF("Extension: %s", ext.extensionName);
	}

	uint32_t availableLayersCount;
	OOVR_FAILED_XR_ABORT(xrEnumerateApiLayerProperties(0, &availableLayersCount, nullptr));
	OOVR_LOGF("Num layers available: %d ", availableLayersCount);
	std::vector<XrApiLayerProperties> layerProperties;
	layerProperties.resize(availableLayersCount, { XR_TYPE_API_LAYER_PROPERTIES });
	OOVR_FAILED_XR_ABORT(xrEnumerateApiLayerProperties(
	    layerProperties.size(), &availableLayersCount, layerProperties.data()));
	std::set<std::string> availableLayers;
	for (const XrApiLayerProperties& layer : layerProperties) {
		availableLayers.insert(layer.layerName);
		OOVR_LOGF("Layer (explicit): %s", layer.layerName);
	}

	// Explicit layers above are only half the story — log the implicit ones too.
	LogImplicitOpenXRLayers();

	// Create the OpenXR instance - this is the overall handle that connects us to the runtime
	// https://www.khronos.org/registry/OpenXR/specs/1.0/refguide/openxr-10-reference-guide.pdf
	XrApplicationInfo appInfo{};
	GetXRAppName(appInfo.applicationName);
	appInfo.applicationVersion = 1;
	appInfo.apiVersion = XR_CURRENT_API_VERSION;

	std::vector<const char*> extensions;
	XrGraphicsApiSupportedFlags apiFlags = 0;

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (availableExtensions.count("XR_KHR_D3D11_enable")) {
		extensions.push_back("XR_KHR_D3D11_enable");
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D11;
	}
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	if (availableExtensions.count("XR_KHR_D3D12_enable")) {
		extensions.push_back("XR_KHR_D3D12_enable");
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_D3D12;
	}
#endif
#if defined(SUPPORT_VK)
	if (availableExtensions.count(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_VK;
	}
#endif
#if defined(SUPPORT_GL)
	if (availableExtensions.count(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_GL;
	}
#endif
#if defined(SUPPORT_GLES)
	if (availableExtensions.count(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
		extensions.push_back(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
		apiFlags |= XR_SUPPORTED_GRAPHICS_API_GLES;
	}
#endif
#if defined(ANDROID)
	if (availableExtensions.count(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
	if (availableExtensions.count(XR_EXT_DEBUG_UTILS_EXTENSION_NAME))
		extensions.push_back(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);

	// If the visibility mask is available use it, otherwise no big deal
	if (availableExtensions.count(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME))
		extensions.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);

	if (availableExtensions.count(XR_EXT_HAND_TRACKING_EXTENSION_NAME))
		extensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

	// Standard cross-runtime gaze input. PSVR2 Toolkit exposes this through
	// SteamVR OpenXR; other runtimes/headsets may expose it directly. Auto eye
	// tracking never enables fixed VRS when gaze is unavailable; Fixed is a
	// separate explicit setting.
	if (oovr_global_configuration.VrsEyeTracked()) {
		if (availableExtensions.count(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
			extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
			xr_extEyeGazeInteraction = true;
			OOVR_LOG("Eye gaze capability: XR_EXT_eye_gaze_interaction advertised and enabled");
		} else {
			OOVR_LOG("Eye gaze capability: requested, but this OpenXR runtime did not advertise XR_EXT_eye_gaze_interaction");
		}
	}

	if (availableExtensions.contains(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME))
		extensions.push_back(XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);

	// Ratified full-featured fallback for controllers without a hardware-specific
	// OpenXR interaction profile. SteamVR's PSVR2 runtime advertises this and maps
	// Sense primary/secondary buttons, thumbsticks, triggers, grips, poses, and
	// haptics into it. This prevents fallback to the input-starved Simple profile.
	if (availableExtensions.count("XR_KHR_generic_controller")) {
		extensions.push_back("XR_KHR_generic_controller");
		xr_khrGenericController = true;
		OOVR_LOG("XR_KHR_generic_controller extension available and enabled");
	}

	// Body trackers: VDXR exposes Virtual Desktop's body tracking through this,
	// SteamVR's OpenXR runtime exposes real Vive/Tundra/SlimeVR trackers the same way
	if (availableExtensions.count("XR_HTCX_vive_tracker_interaction")) {
		extensions.push_back("XR_HTCX_vive_tracker_interaction");
		xr_htcxViveTrackers = true;
		OOVR_LOGF("XR_HTCX_vive_tracker_interaction enabled, revision %u", viveTrackerInteractionVersion);
	}

	const char* const layers[] = {
#ifdef XR_VALIDATION_LAYER_PATH
		"XR_APILAYER_LUNARG_core_validation",
#endif
		nullptr // Dummy value since MSVC gets upset if there's nothing in this array
	};

	XrInstanceCreateInfo createInfo{};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	createInfo.applicationInfo = appInfo;
	createInfo.enabledExtensionNames = extensions.data();
	createInfo.enabledExtensionCount = extensions.size();
	createInfo.enabledApiLayerNames = layers;
	createInfo.enabledApiLayerCount = (sizeof(layers) / sizeof(const char*)) - 1; // Subtract the dummy value

#if ANDROID
	if (!OpenComposite_Android_Create_Info) {
		OOVR_ABORT("Cannot create OpenXR instance - OpenComposite_Android_Create_Info not set");
	}
	XrInstanceCreateInfoAndroidKHR androidInfo = *OpenComposite_Android_Create_Info;
	androidInfo.next = nullptr;
	createInfo.next = &androidInfo;
#endif

	OOVR_FAILED_XR_ABORT(xrCreateInstance(&createInfo, &xr_instance));
	// Record the actual active runtime, not merely the headset/system name. This
	// distinguishes native PimaxXR/Pimax Play from SteamVR's OpenXR bridge when
	// diagnosing eye-gaze behavior.
	XrInstanceProperties instanceProperties{ XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(xr_instance, &instanceProperties))) {
		OOVR_LOGF("OpenXR runtime: %s %u.%u.%u",
		    instanceProperties.runtimeName,
		    XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
		    XR_VERSION_MINOR(instanceProperties.runtimeVersion),
		    XR_VERSION_PATCH(instanceProperties.runtimeVersion));
	}

#ifdef _DEBUG
	XrDebugUtilsMessengerCreateInfoEXT dbgCreateInfo{};
	dbgCreateInfo.type = XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	dbgCreateInfo.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	dbgCreateInfo.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
	dbgCreateInfo.next = NULL;
	dbgCreateInfo.userData = NULL;
	if (dbgMessenger != NULL) {
		OOVR_FAILED_XR_ABORT(xrDestroyDebugUtilsMessengerEXT(dbgMessenger));
		dbgMessenger = NULL;
	}
	dbgCreateInfo.userCallback = debugCallback;
	OOVR_FAILED_XR_ABORT(xrCreateDebugUtilsMessengerEXT(xr_instance, &dbgCreateInfo, &dbgMessenger));
#endif

	// Load the function pointers for the extension functions
	xr_ext = new XrExt(apiFlags, extensions);

	CreateSystemID();

	// List off the views and store them locally for easy access
	uint32_t viewCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateViewConfigurationViews(xr_instance, xr_system,
	    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));

	xr_views_list = std::vector<XrViewConfigurationView>(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });

	OOVR_FAILED_XR_ABORT(xrEnumerateViewConfigurationViews(xr_instance, xr_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    viewCount, &viewCount, xr_views_list.data()));

	OOVR_FALSE_ABORT(viewCount == xr_views_list.size());

	// Create a session - this tells the runtime that sooner or later we'd like to submit frames
	// This is when we have to choose what graphics API to use

	bool useVulkanTmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_VK) && oovr_global_configuration.InitUsingVulkan();
	bool useD3D11TmpGfx = (apiFlags & XR_SUPPORTED_GRAPHICS_API_D3D11);

#if !defined(SUPPORT_VK) && !defined(SUPPORT_DX) && !defined(SUPPORT_DX11)
#error No available temporary graphics implementation
#endif

	// Build a backend that works with OpenXR
	currentBackend = new XrBackend(useVulkanTmpGfx, useD3D11TmpGfx);

	// Setup our OpenXR session
	SetupSession();

	return currentBackend;
}

static void LogLocomotionSessionCheckpoint(const char* stage)
{
	if (!oovr_global_configuration.TreadmillEnabled()) return;
	OOVR_LOGF("Locomotion startup: %s", stage);
	// Preserve the last completed startup phase if the process exits before
	// the normal buffered logger's next flush. No per-frame disk writes.
	oovr_log_flush();
}

void DrvOpenXR::SetupSession()
{
	// SetupSession is used to restart the session, and as such, we want to prevent other threads from attempting to use the session
	// while we're rebuilding it (otherwise we get gross nondescript crashes), so we will put a lock on it here.
	auto lock = xr_session.lock();
	LogLocomotionSessionCheckpoint("session setup entered");
	if (xr_gbl) {
		ShutdownSession();
	}

	XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

	XrSessionCreateInfo sessionInfo{};
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.systemId = xr_system;
	sessionInfo.next = currentBackend->GetCurrentGraphicsBinding();
	OOVR_FAILED_XR_ABORT(xrCreateSession(xr_instance, &sessionInfo, &xr_session.get()));

	// Setup the OpenXR globals, which uses the current session so we have to do this last
	xr_gbl = new XrSessionGlobals();
	LogLocomotionSessionCheckpoint("session and reference spaces created");

	// Print the current version for diagnostic purposes
	OOVR_LOGF("Started OpenXR session on system '%s', hand tracking supported: %d, eye gaze supported: %d",
	    xr_gbl->systemProperties.systemName, xr_gbl->handTrackingProperties.supportsHandTracking,
	    xr_gbl->eyeGazeProperties.supportsEyeGazeInteraction);
	OOVR_LOGF("Eye gaze session capability: requested=%d extensionEnabled=%d systemSupports=%d; foveation backend=%s fixedEnabled=%d",
	    oovr_global_configuration.VrsEyeTracked(), xr_extEyeGazeInteraction,
	    xr_gbl->eyeGazeProperties.supportsEyeGazeInteraction,
	    oovr_global_configuration.FoveatedBackend().c_str(), oovr_global_configuration.VrsFixedEnabled());
	if (oovr_global_configuration.VrsEyeTracked() && xr_extEyeGazeInteraction &&
	    !xr_gbl->eyeGazeProperties.supportsEyeGazeInteraction) {
		OOVR_LOG("Eye gaze unavailable: extension enabled, but the active system reports no eye-gaze support. Check headset eye-tracking hardware, calibration and the active runtime/driver's gaze forwarding.");
	}

	// Attach inputs early so implicit layers (like VD) see an attached
	// action set before the first xrSyncActions / xrWaitFrame.
	auto inputShared = GetBaseInput();
	if (!inputShared) {
		inputShared = GetCreateBaseInput();
	}
	sessionInputKeepalive = inputShared;
	BaseInput* input = inputShared.get();
	if (input) {
		if (!input->AreActionsLoaded()) {
			input->LoadEmptyManifestIfRequired(false);
		} else {
			input->BindInputsForSession();
		}
	}

	LogLocomotionSessionCheckpoint("input setup completed; processing session events");
	currentBackend->OnSessionCreated();
	LogLocomotionSessionCheckpoint("session event processing completed");
}

void DrvOpenXR::ShutdownSession()
{
	OOVR_DEBUG_LOGF("[INPUT-TRACE] Session shutdown requested session=%p", (void*)xr_session.get());
	BackendManager* instance = BackendManager::InstancePtr();
	// Is it already being shut down?
	// Note that this is indirectly called by the XrBackend destructor, which will have
	// already called this function in that case.
	if (instance) {
		auto* backend = (XrBackend*)instance->GetBackendInstance();
		if (backend)
			backend->PrepareForSessionShutdown();
	}

	if (currentBackend->sessionActive) {
		// Hey it turns out that xrDestroySession can be called whenever - how convenient
		OOVR_FAILED_XR_ABORT(xrRequestExitSession(xr_session.get()));
		int count = 0;
		while (currentBackend->GetSessionState() != XR_SESSION_STATE_EXITING && count++ < 10) {
			OOVR_LOGF("currentBackend: %d", currentBackend->GetSessionState());
			const int durationMs = 250;
			OOVR_LOGF("Session Exit state has not been reached yet, waiting %dms ...", durationMs);
#ifdef _WIN32
			Sleep(durationMs);
#else
			struct timespec ts = { 0, durationMs * 1000000 };
			nanosleep(&ts, &ts);
#endif
			currentBackend->PumpEvents();
		}
	}

	OOVR_FAILED_XR_ABORT(xrDestroySession(xr_session.get()));
	OOVR_DEBUG_LOGF("[INPUT-TRACE] Session destroyed session=%p", (void*)xr_session.get());
	xr_session.reset();

	// Delete xr_gbl AFTER session is fully destroyed — PumpEvents() and other
	// shutdown code may still reference xr_gbl->seatedSpace, nextPredictedFrameTime, etc.
	delete xr_gbl;
	xr_gbl = nullptr;

	CreateSystemID();
}

void DrvOpenXR::FullShutdown()
{
	if (xr_session.get())
		ShutdownSession();

#ifdef _DEBUG
	if (dbgMessenger != NULL) {
		OOVR_FAILED_XR_ABORT(xrDestroyDebugUtilsMessengerEXT(dbgMessenger));
		dbgMessenger = NULL;
	}
#endif

	if (xr_instance) {
		OOVR_FAILED_XR_ABORT(xrDestroyInstance(xr_instance));
		xr_instance = XR_NULL_HANDLE;
	}

	initialised = false;
	viveTrackerInteractionVersion = 0;
	xr_htcxViveTrackers = false;
	currentBackend = nullptr;
	sessionInputKeepalive.reset();
}
