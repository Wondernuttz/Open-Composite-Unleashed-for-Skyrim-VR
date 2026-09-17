#include "ASWProvider.h"
#include "DapaCaptureControl.h"
#include "DapaComputeState.h"

#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/logging.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <chrono>

// Global instance — accessed from XrBackend for frame injection
ASWProvider* g_aswProvider = nullptr;

DapaGpuTiming::Scope ASWProvider::MeasureGpu(ID3D11DeviceContext* ctx, DapaGpuTiming::Stage stage)
{
	const bool enabled = oovr_global_configuration.DebugLogging();
	if (enabled) {
		m_gpuTiming.Poll(ctx, [this](const DapaGpuTiming::Result& result) {
			if (result.valid)
				OOVR_LOGF("DAPA GPU SAMPLE v1: stage=%s gpuElapsed=%.3fms resultObservedAfter=%llums eye=%ux%u depth=%ux%u (sparse GPU timestamps; observation delay includes polling, not compositor/GPU queue latency)",
				    DapaGpuTiming::Name(result.stage), result.gpuMs, (unsigned long long)result.readyObservedAfterMs,
				    m_eyeWidth, m_eyeHeight, m_depthWidth, m_depthHeight);
			else
				OOVR_LOGF("DAPA GPU SAMPLE v1: stage=%s unavailable/disjoint status=0x%08X; no GPU duration inferred",
				    DapaGpuTiming::Name(result.stage), (unsigned int)result.status);
		});
		if (m_gpuTiming.Unavailable())
			OOVR_LOG_LIMITEDF(60000, "DAPA GPU SAMPLE v1: timestamp queries unsupported; CPU call times remain separate");
	}
	return m_gpuTiming.Measure(ctx, stage, enabled);
}

// ============================================================================
// Embedded HLSL compute shader for frame warping
// ============================================================================
#include "DapaWarpShader.h"

// ============================================================================
// Quaternion math helpers
// ============================================================================

void ASWProvider::QuatInverse(const XrQuaternionf& q, XrQuaternionf& out)
{
	// For unit quaternions, inverse = conjugate
	out.x = -q.x;
	out.y = -q.y;
	out.z = -q.z;
	out.w = q.w;
}

void ASWProvider::QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b, XrQuaternionf& out)
{
	out.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
	out.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
	out.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
	out.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
}

void ASWProvider::QuatRotateVec(const XrQuaternionf& q, const XrVector3f& v, XrVector3f& out)
{
	// Rotate vector by quaternion: q * v * q^-1
	// Optimized formula: out = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
	float cx = q.y * v.z - q.z * v.y + q.w * v.x;
	float cy = q.z * v.x - q.x * v.z + q.w * v.y;
	float cz = q.x * v.y - q.y * v.x + q.w * v.z;
	out.x = v.x + 2.0f * (q.y * cz - q.z * cy);
	out.y = v.y + 2.0f * (q.z * cx - q.x * cz);
	out.z = v.z + 2.0f * (q.x * cy - q.y * cx);
}

void ASWProvider::BuildPoseDeltaMatrix(const XrPosef& oldPose, const XrPosef& newPose,
    float* m)
{
	// Build the 4x4 matrix that transforms view-space positions from oldPose to newPose.
	// This is: newView * inverse(oldView)
	// Which simplifies to: deltaRot applied to position, then deltaTranslation
	//
	// deltaRotation = inverse(newPose.orientation) * oldPose.orientation
	// (because view matrix inverts the pose: viewRot = inverse(poseRot))
	//
	// Actually: oldView = inverse(oldPose), newView = inverse(newPose)
	// poseDelta = newView * inverse(oldView) = inverse(newPose) * oldPose
	//
	// For the rotation part:
	//   deltaRot = conjugate(newOri) * oldOri
	// For the translation part:
	//   deltaPos in new view space = conjugate(newOri) * (oldPos - newPos)

	XrQuaternionf newOriInv;
	QuatInverse(newPose.orientation, newOriInv);

	// Rotation: conjugate(new) * old
	XrQuaternionf deltaRot;
	QuatMultiply(newOriInv, oldPose.orientation, deltaRot);

	// Translation: conjugate(new) * (oldPos - newPos)
	XrVector3f posDiff = {
		oldPose.position.x - newPose.position.x,
		oldPose.position.y - newPose.position.y,
		oldPose.position.z - newPose.position.z
	};
	XrVector3f deltaTrans;
	QuatRotateVec(newOriInv, posDiff, deltaTrans);

	// Convert quaternion to 3x3 rotation matrix (row-major)
	float qx = deltaRot.x, qy = deltaRot.y, qz = deltaRot.z, qw = deltaRot.w;
	float xx = qx * qx, yy = qy * qy, zz = qz * qz;
	float xy = qx * qy, xz = qx * qz, yz = qy * qz;
	float wx = qw * qx, wy = qw * qy, wz = qw * qz;

	// Row-major 4x4 matrix
	m[0]  = 1.0f - 2.0f * (yy + zz);  m[1]  = 2.0f * (xy - wz);          m[2]  = 2.0f * (xz + wy);          m[3]  = deltaTrans.x;
	m[4]  = 2.0f * (xy + wz);          m[5]  = 1.0f - 2.0f * (xx + zz);   m[6]  = 2.0f * (yz - wx);          m[7]  = deltaTrans.y;
	m[8]  = 2.0f * (xz - wy);          m[9]  = 2.0f * (yz + wx);          m[10] = 1.0f - 2.0f * (xx + yy);   m[11] = deltaTrans.z;
	m[12] = 0.0f;                       m[13] = 0.0f;                       m[14] = 0.0f;                       m[15] = 1.0f;
}

// ============================================================================
// Lifecycle
// ============================================================================

ASWProvider::~ASWProvider()
{
	Shutdown();
}

bool ASWProvider::Initialize(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight)
{
	if (m_ready) return true;
	if (!device || eyeWidth == 0 || eyeHeight == 0) return false;

	OOVR_LOGF("ASW: Initializing — per-eye %ux%u", eyeWidth, eyeHeight);

	m_eyeWidth = eyeWidth;
	m_eyeHeight = eyeHeight;
	m_device = device;
	m_device->AddRef(); // prevent device destruction while ASW holds a reference

	if (!CreateComputeShader(device)) {
		OOVR_LOG("ASW: Failed to create compute shader");
		Shutdown();
		return false;
	}

	if (!CreateStagingTextures(device)) {
		OOVR_LOG("ASW: Failed to create staging textures");
		Shutdown();
		return false;
	}

	if (!CreateOutputSwapchain(eyeWidth * 2, eyeHeight)) {
		OOVR_LOG("ASW: Failed to create output swapchain");
		Shutdown();
		return false;
	}

	if (!CreateDepthSwapchain(eyeWidth * 2, eyeHeight)) {
		OOVR_LOG("ASW: Failed to create depth swapchain (non-fatal — depth layer disabled)");
		// Non-fatal: ASW works without depth, just no depth attachment for the runtime
	}

	m_ready = true;
	InvalidateCachedFrame();
	OOVR_LOGF("ASW: Initialized — %ux%u per eye, compute shader ready", eyeWidth, eyeHeight);
	OOVR_LOG("DAPA: compute-state-restore-v1 / depth-read-hazard-v1");
	return true;
}

bool ASWProvider::CreateComputeShader(ID3D11Device* device)
{
	// Compile HLSL
	DWORD flags = D3DCOMPILE_PACK_MATRIX_ROW_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = D3DCompile(s_warpShaderHLSL, strlen(s_warpShaderHLSL),
	    "ASWWarp", nullptr, nullptr, "CSMain", "cs_5_0", flags, 0, &compiled, &errors);
	if (FAILED(hr)) {
		if (errors) {
			OOVR_LOGF("ASW: Shader compile error: %s", (char*)errors->GetBufferPointer());
			errors->Release();
		}
		return false;
	}
	if (errors) errors->Release();

	hr = device->CreateComputeShader(compiled->GetBufferPointer(),
	    compiled->GetBufferSize(), nullptr, &m_warpCS);
	compiled->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateComputeShader failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	// Constant buffer
	D3D11_BUFFER_DESC cbDesc = {};
	cbDesc.ByteWidth = sizeof(WarpConstants);
	// Pad to 16-byte alignment (WarpConstants is 272 bytes, already aligned)
	cbDesc.ByteWidth = (cbDesc.ByteWidth + 15) & ~15;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateBuffer (CB) failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	cbDesc.ByteWidth = sizeof(BlackoutConstants);
	m_uploadedBlackout = {};
	D3D11_SUBRESOURCE_DATA initialBlackout = { &m_uploadedBlackout, 0, 0 };
	hr = device->CreateBuffer(&cbDesc, &initialBlackout, &m_blackoutConstantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateBuffer (blackout CB) failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	// Linear clamp sampler
	D3D11_SAMPLER_DESC sampDesc = {};
	sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	hr = device->CreateSamplerState(&sampDesc, &m_linearSampler);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateSamplerState failed hr=0x%08x", (unsigned)hr);
		return false;
	}

	OOVR_LOG("ASW: Compute shader compiled and ready");
	return true;
}

bool ASWProvider::CreateStagingTextures(ID3D11Device* device)
{
	for (int eye = 0; eye < 2; eye++) {
		// Cached color (RGBA)
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = m_eyeWidth;
		desc.Height = m_eyeHeight;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;

		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &m_cachedColor[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateTexture2D color[%d] failed", eye); return false; }

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		hr = device->CreateShaderResourceView(m_cachedColor[eye], &srvDesc, &m_srvColor[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateSRV color[%d] failed", eye); return false; }

		// Cached MV (R16G16_FLOAT)
		desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		hr = device->CreateTexture2D(&desc, nullptr, &m_cachedMV[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateTexture2D MV[%d] failed", eye); return false; }

		srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		hr = device->CreateShaderResourceView(m_cachedMV[eye], &srvDesc, &m_srvMV[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateSRV MV[%d] failed", eye); return false; }

		// Cached depth (R32_FLOAT)
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		hr = device->CreateTexture2D(&desc, nullptr, &m_cachedDepth[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateTexture2D depth[%d] failed", eye); return false; }

		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		hr = device->CreateShaderResourceView(m_cachedDepth[eye], &srvDesc, &m_srvDepth[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateSRV depth[%d] failed", eye); return false; }

		// Warped output (RGBA, UAV for compute shader)
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		hr = device->CreateTexture2D(&desc, nullptr, &m_warpedOutput[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateTexture2D output[%d] failed", eye); return false; }

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(m_warpedOutput[eye], &uavDesc, &m_uavOutput[eye]);
		if (FAILED(hr)) { OOVR_LOGF("ASW: CreateUAV output[%d] failed", eye); return false; }
	}

	m_depthWidth = m_eyeWidth;
	m_depthHeight = m_eyeHeight;
	m_depthLayerValid = true;
	OOVR_LOG("ASW: Staging textures created (2 eyes × 4 textures)");
	return true;
}

void ASWProvider::ReleaseStagingTextures()
{
	for (int i = 0; i < 2; i++) {
		if (m_uavOutput[i]) { m_uavOutput[i]->Release(); m_uavOutput[i] = nullptr; }
		if (m_warpedOutput[i]) { m_warpedOutput[i]->Release(); m_warpedOutput[i] = nullptr; }
		if (m_srvDepth[i]) { m_srvDepth[i]->Release(); m_srvDepth[i] = nullptr; }
		if (m_srvMV[i]) { m_srvMV[i]->Release(); m_srvMV[i] = nullptr; }
		if (m_srvColor[i]) { m_srvColor[i]->Release(); m_srvColor[i] = nullptr; }
		if (m_cachedDepth[i]) { m_cachedDepth[i]->Release(); m_cachedDepth[i] = nullptr; }
		if (m_cachedMV[i]) { m_cachedMV[i]->Release(); m_cachedMV[i] = nullptr; }
		if (m_cachedColor[i]) { m_cachedColor[i]->Release(); m_cachedColor[i] = nullptr; }
	}
}

// Adopt a new submitted-frame size (external render-scale mods upscale the
// game's frame to display res at submit). Recreates the staging textures and
// both XR swapchains at the new size. Depth is re-fitted afterwards by
// ResizeDepthCache if the game's depth target differs.
bool ASWProvider::ResizeColorPath(uint32_t eyeW, uint32_t eyeH)
{
	m_outputLease = {};
	m_depthLease = {};
	m_depthSubmittedThisFrame = false;
	OOVR_LOGF("ASW: adaptive resize color path %ux%u -> %ux%u",
	    m_eyeWidth, m_eyeHeight, eyeW, eyeH);

	InvalidateCachedFrame();
	ReleaseStagingTextures();

	if (m_outputSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_outputSwapchain);
		m_outputSwapchain = {};
	}
	m_outputSwapchainImages.clear();
	if (m_depthSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = {};
	}
	m_depthSwapchainImages.clear();

	m_eyeWidth = eyeW;
	m_eyeHeight = eyeH;

	if (!CreateStagingTextures(m_device)) {
		OOVR_LOG("ASW: adaptive resize FAILED (staging) — disabling");
		m_ready = false;
		return false;
	}
	if (!CreateOutputSwapchain(eyeW * 2, eyeH)) {
		OOVR_LOG("ASW: adaptive resize FAILED (output swapchain) — disabling");
		m_ready = false;
		return false;
	}
	if (!CreateDepthSwapchain(eyeW * 2, eyeH)) {
		// Depth layer is optional — keep going without it
		OOVR_LOG("ASW: adaptive resize: depth swapchain recreation failed (depth layer disabled)");
	}
	return true;
}

// Re-fit the depth staging cache to the game's depth target size (may be
// smaller than the eye size under external render scale — the warp shader
// samples depth by UV, so mixed sizes are fine).
bool ASWProvider::ResizeDepthCache(uint32_t w, uint32_t h)
{
	OOVR_LOGF("ASW: adaptive resize depth cache %ux%u -> %ux%u",
	    m_depthWidth, m_depthHeight, w, h);

	InvalidateCachedFrame();
	for (int i = 0; i < 2; i++) {
		if (m_srvDepth[i]) { m_srvDepth[i]->Release(); m_srvDepth[i] = nullptr; }
		if (m_cachedDepth[i]) { m_cachedDepth[i]->Release(); m_cachedDepth[i] = nullptr; }
	}

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = w;
	desc.Height = h;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.Format = DXGI_FORMAT_R32_FLOAT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	for (int i = 0; i < 2; i++) {
		HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_cachedDepth[i]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: depth cache resize FAILED (tex %d)", i);
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		hr = m_device->CreateShaderResourceView(m_cachedDepth[i], &srvDesc, &m_srvDepth[i]);
		if (FAILED(hr)) {
			OOVR_LOGF("ASW: depth cache resize FAILED (srv %d)", i);
			return false;
		}
	}
	m_depthWidth = w;
	m_depthHeight = h;
	return true;
}

bool ASWProvider::CreateOutputSwapchain(uint32_t width, uint32_t height)
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT
	    | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	XrResult res = xrCreateSwapchain(xr_session.get(), &ci, &m_outputSwapchain);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW: xrCreateSwapchain failed (%ux%u) result=%d", width, height, (int)res);
		return false;
	}

	// Enumerate images — cache first
	uint32_t imageCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_outputSwapchain, 0, &imageCount, nullptr));
	if (imageCount == 0) {
		OOVR_LOG("ASW: Output swapchain has 0 images");
		xrDestroySwapchain(m_outputSwapchain);
		m_outputSwapchain = {};
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_outputSwapchain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()));

	m_outputSwapchainImages.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++)
		m_outputSwapchainImages[i] = images[i].texture;

	OOVR_LOGF("ASW: Output swapchain created %ux%u (%u images)", width, height, imageCount);
	return true;
}

XrRect2Di ASWProvider::GetOutputRect(int eye) const
{
	XrRect2Di rect = {};
	rect.offset.x = eye * (int32_t)m_eyeWidth;
	rect.offset.y = 0;
	rect.extent.width = (int32_t)m_eyeWidth;
	rect.extent.height = (int32_t)m_eyeHeight;
	return rect;
}

bool ASWProvider::CreateDepthSwapchain(uint32_t width, uint32_t height)
{
	m_depthTransfer.Reset();
	uint32_t formatCount = 0;
	XrResult res = xrEnumerateSwapchainFormats(xr_session.get(), 0, &formatCount, nullptr);
	if (res != XR_SUCCESS || !formatCount) return false;
	std::vector<int64_t> formats(formatCount);
	res = xrEnumerateSwapchainFormats(xr_session.get(), formatCount, &formatCount, formats.data());
	if (res != XR_SUCCESS) return false;
	formats.resize(formatCount);
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	// Standard depth first; retain R32_FLOAT compatibility where advertised.
	for (auto format : { DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT }) {
		if (std::find(formats.begin(), formats.end(), int64_t(format)) == formats.end()) continue;
		ci.format = format;
		ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		if (format == DXGI_FORMAT_D32_FLOAT) ci.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		res = xrCreateSwapchain(xr_session.get(), &ci, &m_depthSwapchain);
		if (res == XR_SUCCESS) break;
		m_depthSwapchain = XR_NULL_HANDLE;
		OOVR_LOGF("ASW: depth format=%d rejected result=%d; trying next advertised format", int(format), int(res));
	}
	if (m_depthSwapchain == XR_NULL_HANDLE) {
		OOVR_LOG("ASW: no supported D32_FLOAT/R32_FLOAT depth swapchain; continuing with PC-side depth");
		return false;
	}
	auto discard = [&]() {
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = XR_NULL_HANDLE;
		m_depthSwapchainImages.clear();
		return false;
	};

	uint32_t imageCount = 0;
	res = xrEnumerateSwapchainImages(m_depthSwapchain, 0, &imageCount, nullptr);
	if (res != XR_SUCCESS || imageCount == 0) {
		OOVR_LOG("ASW: Depth swapchain has 0 images");
		return discard();
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	res = xrEnumerateSwapchainImages(m_depthSwapchain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data());
	if (res != XR_SUCCESS || !imageCount || imageCount > images.size()) return discard();

	m_depthSwapchainImages.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++)
		m_depthSwapchainImages[i] = images[i].texture;

	OOVR_LOGF("ASW: Depth transfer v2: swapchain %ux%u format=%lld (%u images), whole-atlas copy", width, height, (long long)ci.format, imageCount);
	return true;
}

// ============================================================================
// Per-frame operations
// ============================================================================

// SEH wrapper for CopySubresourceRegion — catches AV from TOCTOU race
// when bridge texture is freed between validation and copy.
// Must be a standalone function: __try/__except can't coexist with C++ destructors.
static bool SafeBridgeCopy(ID3D11DeviceContext* ctx,
    ID3D11Resource* dst, UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* srcBox) {
	__try {
		ctx->CopySubresourceRegion(dst, dstSub, dstX, dstY, dstZ, src, srcSub, srcBox);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
	    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
		return false;
	}
}

bool ASWProvider::CacheFrame(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
    bool sourceFlipV,
    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
    const XrPosef& eyePose, const XrFovf& eyeFov,
    float nearZ, float farZ, ID3D11Texture2D* bodyDepthMask,
    const ocu_foveation::BlackoutFrame& blackout)
{
	auto failGeneration = [this]() {
		InvalidateCachedFrame();
		return false;
	};

	if (!m_ready || m_paused || eye < 0 || eye > 1)
		return failGeneration();

	// A left-eye submit begins a new generation. The provider has a single
	// stereo cache, so the previously published pair must stop being visible
	// before either eye is overwritten.
	if (eye == 0) {
		// Normal pair turnover preserves REAL-frame motion history. Error / pause /
		// resize invalidation still resets it through InvalidateCachedFrame().
		m_hasCachedFrame = false;
		m_cacheBuildEyeMask = 0;
		m_cachedBlackout[0] = m_cachedBlackout[1] = {};
		m_motionGeometryValid[0] = m_motionGeometryValid[1] = false;
	} else if (m_cacheBuildEyeMask != 0x1) {
		// Never combine a right eye with a left eye from an older generation.
		return failGeneration();
	}

	auto validRegion = [](const D3D11_BOX* region) {
		return region
		    && region->right > region->left
		    && region->bottom > region->top
		    && region->back > region->front;
	};
	if (!ctx || !colorTex || !depthTex
	    || !validRegion(colorRegion) || !validRegion(depthRegion)) {
		return failGeneration();
	}
	DapaPredicationState unpredicated(ctx);

	// ── Adaptive sizing ──
	// External render-scale mods (Community Shaders VR) upscale the submitted
	// frame to display res while the game's depth target stays at render res,
	// and both can change size mid-session ("relatch"). Follow the sources:
	// the color/output path adopts the submitted size, the depth cache adopts
	// the depth target size, and the warp shader samples depth by UV.
	if (colorRegion) {
		uint32_t cw = colorRegion->right - colorRegion->left;
		uint32_t ch = colorRegion->bottom - colorRegion->top;
		if (cw && ch && (cw != m_eyeWidth || ch != m_eyeHeight)) {
			if (eye != 0)
				return failGeneration(); // resize only on eye 0 so the pair stays consistent
			if (!ResizeColorPath(cw, ch))
				return failGeneration();
		}
	}
	if (depthRegion) {
		uint32_t dw = depthRegion->right - depthRegion->left;
		uint32_t dh = depthRegion->bottom - depthRegion->top;
		if (dw && dh && (dw != m_depthWidth || dh != m_depthHeight)) {
			if (eye != 0)
				return failGeneration();
			if (!ResizeDepthCache(dw, dh))
				return failGeneration();
		}
	}
	if (!m_cachedColor[eye] || !m_cachedDepth[eye])
		return failGeneration();
	// The complete inverse solve, including foreground seeds and bilinear
	// colour support, must fit within the scene's retained sampling guard.
	if ((blackout.mask.Active() && !blackout.Active()) ||
	    !ocu_foveation::BlackoutSupportsDapaSource(blackout, eye, m_eyeWidth, m_eyeHeight)) {
		OOVR_LOG_LIMITEDF(5000, "DAPA blackout cache withheld: eye=%d source=%ux%u scene=%.0fx%.0f guard=%.1fpx; awaiting a frame with sufficient source support",
		    eye, m_eyeWidth, m_eyeHeight, blackout.sceneEyeSize[eye][0],
		    blackout.sceneEyeSize[eye][1], blackout.mask.guardPixels);
		return failGeneration();
	}
	if (eye == 1 && (m_cachedBlackout[0].mask.Active() || blackout.mask.Active())) {
		if (!m_cachedBlackout[0].Active() || !blackout.Active() ||
		    m_cachedBlackout[0].frameId != blackout.frameId)
			return failGeneration();
	}

	// The XR depth layer needs depth at the eye size; the parallax warp does not.
	m_depthLayerValid = (m_depthWidth == m_eyeWidth && m_depthHeight == m_eyeHeight);

	// Copy color (game eye texture → cached)
	auto gpuSample = MeasureGpu(ctx, eye == 0 ? DapaGpuTiming::Stage::CacheLeft : DapaGpuTiming::Stage::CacheRight);
	if (colorTex && colorRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedColor[eye], 0, 0, 0, 0,
		    colorTex, 0, colorRegion)) {
			OOVR_LOG("ASW: TOCTOU — color texture freed during copy");
			return failGeneration();
		}
	}

	// MV copy skipped — parallax-only warp shader never samples t1
	(void)mvTex;
	(void)mvRegion;

	// Copy depth (bridge depth → cached)
	if (depthTex && depthRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedDepth[eye], 0, 0, 0, 0,
		    depthTex, 0, depthRegion)) {
			OOVR_LOG("ASW: TOCTOU — depth texture freed during copy");
			return failGeneration();
		}
	}

	// A mask is usable only for this source eye, source resolution and device.
	// The caller holds the shared mask lease before passing this pointer and
	// until this copy is queued, including every COM method below.
	m_bodyValid[eye] = false;
	if (bodyDepthMask) {
		D3D11_TEXTURE2D_DESC bd{},dd{};
		bodyDepthMask->GetDesc(&bd); depthTex->GetDesc(&dd);
		Microsoft::WRL::ComPtr<ID3D11Device> owner;
		bodyDepthMask->GetDevice(&owner);
		if (owner.Get()==m_device && bd.Format==DXGI_FORMAT_R32_FLOAT &&
		    bd.Width==dd.Width && bd.Height==dd.Height && bd.ArraySize==1 && bd.SampleDesc.Count==1) {
			D3D11_TEXTURE2D_DESC cached{};
			if(m_bodyDepth[eye])m_bodyDepth[eye]->GetDesc(&cached);
			if(!m_bodySrv[eye] || cached.Width!=m_depthWidth || cached.Height!=m_depthHeight) {
				m_bodySrv[eye].Reset();m_bodyDepth[eye].Reset();
				bd.Width=m_depthWidth;bd.Height=m_depthHeight;bd.MipLevels=1;
				bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.Usage=D3D11_USAGE_DEFAULT;
				bd.CPUAccessFlags=bd.MiscFlags=0;
				if(SUCCEEDED(m_device->CreateTexture2D(&bd,nullptr,&m_bodyDepth[eye])))
					m_device->CreateShaderResourceView(m_bodyDepth[eye].Get(),nullptr,&m_bodySrv[eye]);
			}
			if(m_bodySrv[eye])m_bodyValid[eye]=SafeBridgeCopy(ctx,m_bodyDepth[eye].Get(),0,0,0,0,bodyDepthMask,0,depthRegion);
		}
		if (!m_bodyValid[eye])
			return failGeneration(); // A required player mask must never disappear silently.
	}
	m_cachedPose[eye] = eyePose;
	m_cachedFov[eye] = eyeFov;
	m_cachedSourceFlipV[eye] = sourceFlipV;
	m_cachedBlackout[eye] = blackout;
	// Cached colour remains in source texture orientation; the warp samples it
	// through RawUV. Express the scene mask in the warp's canonical output UV.
	if (sourceFlipV && m_cachedBlackout[eye].Active())
		m_cachedBlackout[eye].centers[eye][1] = 1.f - blackout.centers[eye][1];
	m_cachedNear = nearZ;
	m_cachedFar = farZ;

	m_cacheBuildEyeMask |= static_cast<uint8_t>(1u << eye);

	// Publish only a complete left+right pair from this generation.
	if (eye == 1) {
		if (m_cacheBuildEyeMask != 0x3)
			return failGeneration();
		m_hasCachedFrame = true;
		m_cacheBuildEyeMask = 0;
		static int s = 0;
		if (s++ < 3)
			OOVR_LOGF("ASW: Frame cached — near=%.2f far=%.1f", nearZ, farZ);
	}
	return true;
}

void ASWProvider::SampleLocomotion(XrTime time, DapaMotion::Vec3 position)
{
	m_motion.Sample(time, position);
	if constexpr (!DapaCaptureControl::Enabled) return;
	// Independent raw actor displacement: no predictor filtering/confidence/clamp.
	m_captureMovement.SamplePosition(time,DapaCaptureTelemetry::NowNs(),position);
	m_captureMovement.latest.predictorVelocity=m_motion.velocity;
	m_captureMovement.latest.predictorConfidence=m_motion.confidence;
	m_captureMovement.latest.actorYaw=m_turn.yaw;
	m_captureMovement.latest.turnRate=m_turn.rate;
	m_captureMovement.latest.turnConfidence=m_turn.confidence;
	m_captureMovement.latest.turnInput=m_turn.turning;
	m_captureMovement.latest.turnValid=m_turn.haveRate;
	for(int eye=0;eye<2;++eye)if(m_motionGeometryValid[eye])
		m_captureMovement.SetView(eye,m_motionView[eye],m_motionProjection[eye][11]>0?1.0f:-1.0f);
	m_captureMovement.latest.sticks=DapaCaptureTelemetry::Read();
	m_capture.ObserveMovement(m_captureMovement.latest);
	if (m_device && m_hasCachedFrame && m_capture.NeedsNext()) {
		ID3D11DeviceContext* ctx = nullptr;
		m_device->GetImmediateContext(&ctx);
		m_capture.NextPair(ctx, m_cachedColor, time, m_cachedSourceFlipV,
		    reinterpret_cast<const float*>(&m_cachedPose[0]), reinterpret_cast<const float*>(&m_cachedPose[1]));
		ctx->Release();
	}
}

void ASWProvider::CaptureTick()
{
	if constexpr (!DapaCaptureControl::Enabled) return;
	m_capture.Poll();
	if (m_device && m_capture.NeedsPump()) {
		ID3D11DeviceContext* ctx = nullptr;
		m_device->GetImmediateContext(&ctx);
		m_capture.Pump(ctx);
		ctx->Release();
	}
}

void ASWProvider::SetMotionGeometry(int eye, const float* view, const float* vp)
{
	if (eye < 0 || eye > 1) return;
	m_motionGeometryValid[eye] = view && vp && DapaMotion::Projection(view, vp,
	    m_motionProjection[eye], m_motionInvProjection[eye]);
	if (m_motionGeometryValid[eye]) memcpy(m_motionView[eye], view, sizeof(m_motionView[eye]));
}

bool ASWProvider::WarpFrame(int eye, ID3D11DeviceContext* ctx,
    const XrPosef& newPose)
{
	if (!ctx || !m_ready || m_paused || !m_hasCachedFrame || eye < 0 || eye > 1) return false;
	// Includes optional capture copies that precede the compute-state guard.
	DapaPredicationState unpredicated(ctx);

	// Build pose delta matrix
	WarpConstants cb = {};
	// Backward warping: matrix transforms NEW view → OLD view
	// so we can find where each output pixel maps to in the cached frame
	BuildPoseDeltaMatrix(newPose, m_cachedPose[eye], cb.poseDeltaMatrix);

	// Tracked HEAD rotation is left to the runtime. Game stick-turn rotation
	// is a separate actor-heading signal, applied below in each cached eye basis.
	// Force rotation part (3x3 upper-left) to identity:
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			cb.poseDeltaMatrix[r * 4 + c] = (r == c) ? 1.0f : 0.0f;

	const float master = oovr_global_configuration.ASWWarpStrength();
	const float rotS = oovr_global_configuration.ASWRotationScale();
	const float predictedYaw=m_turn.Predict(m_warpDisplayTime);
	const float theta=std::clamp(predictedYaw*rotS*master,-0.12f,0.12f);
	bool turnApplied=false;
	if(m_motionGeometryValid[eye] && std::isfinite(theta) && theta!=0)
		turnApplied=DapaMotion::WorldYawToView(theta,m_motionView[eye],cb.poseDeltaMatrix);

	// BuildPoseDeltaMatrix(new,cached) gives cached^-1 * (new-cached): the
	// correct backward-lookup translation, in OpenXR metres (-Z forward).
	// Convert to the actual game view's scale and forward convention. This is
	// separate from actor translation: no NiCamera/HMD term enters locomotion.
	float transS = master * oovr_global_configuration.ASWTranslationScale() * 72.0f;
	transS = (transS < 0.0f) ? 0.0f : transS;
	if (m_motionGeometryValid[eye]) {
		const float* v = m_motionView[eye];
		transS *= std::sqrt(v[0]*v[0] + v[4]*v[4] + v[8]*v[8]);
	} else transS = 0;
	cb.poseDeltaMatrix[3] *= transS;
	cb.poseDeltaMatrix[7] *= transS;
	cb.poseDeltaMatrix[11] *= transS * (m_motionProjection[eye][11] > 0 ? -1.0f : 1.0f);

	// Guarded world-space velocity extrapolated to the requested XR display time,
	// transformed by this eye's cached game view (including game world scale).
	if (m_motionGeometryValid[eye]) {
		const auto world = m_motion.Predict(m_warpDisplayTime);
		const auto view = DapaMotion::ToView(world, m_motionView[eye]);
		const float scale = master * oovr_global_configuration.ASWLocoScale();
		// Backward lookup: new camera position = old + delta, hence add delta
		// to the new-view point to find its location in the old camera frame.
		cb.poseDeltaMatrix[3] += view.x * scale;
		cb.poseDeltaMatrix[7] += view.y * scale;
		cb.poseDeltaMatrix[11] += view.z * scale;
	}

	cb.resolution[0] = (float)m_eyeWidth;
	cb.resolution[1] = (float)m_eyeHeight;
	cb.nearZ = m_cachedNear;
	cb.farZ = m_cachedFar;
	cb.fovTanLeft = tanf(m_cachedFov[eye].angleLeft);
	cb.fovTanRight = tanf(m_cachedFov[eye].angleRight);
	cb.fovTanUp = tanf(m_cachedFov[eye].angleUp);
	cb.fovTanDown = tanf(m_cachedFov[eye].angleDown);
	float ds = oovr_global_configuration.ASWDepthScale();
	cb.depthScale = (ds < 0.0f) ? 0.0f : ds;
	cb.edgeFadeWidth = oovr_global_configuration.ASWEdgeFadeWidth();
	cb.nearFadeDepth = oovr_global_configuration.ASWNearFadeDepth() * 72.0f; // meters → game units
	cb.debugTint = (oovr_global_configuration.ASWDebugMode() == 10) ? 1.0f : 0.0f;
	cb.depthResolution[0] = (float)(m_depthWidth ? m_depthWidth : m_eyeWidth);
	cb.depthResolution[1] = (float)(m_depthHeight ? m_depthHeight : m_eyeHeight);
	cb.sourceFlip[0] = 0.0f;
	cb.sourceFlip[1] = m_cachedSourceFlipV[eye] ? 1.0f : 0.0f;
	float movement=std::abs(cb.poseDeltaMatrix[3])+std::abs(cb.poseDeltaMatrix[7])+std::abs(cb.poseDeltaMatrix[11]);
	for(int r=0;r<3;++r)for(int c=0;c<3;++c)movement+=std::abs(cb.poseDeltaMatrix[r*4+c]-(r==c?1.0f:0.0f));
	const bool moving=movement>1e-7f;
	cb.predictionValid = m_motionGeometryValid[eye] && moving ? 1.0f : 0.0f;
	cb.padding[0] = m_bodyValid[eye] ? 3.0f : 2.0f; // v3 adds player-owned source-depth mask at t1
	cb.padding[1] = turnApplied ? theta : 0; // Applied backward world yaw, radians.
	cb.padding[2] = m_turn.confidence;
	m_warpMoved[eye] = cb.predictionValid > 0.5f;
	memcpy(cb.projection, m_motionProjection[eye], sizeof(cb.projection));
	memcpy(cb.inverseProjection, m_motionInvProjection[eye], sizeof(cb.inverseProjection));
	if (eye == 1 && (oovr_global_configuration.DebugLogging() || m_capture.Recording())) {
		static auto lastLog = std::chrono::steady_clock::now() - std::chrono::seconds(3);
		const auto now = std::chrono::steady_clock::now();
		if (now - lastLog >= std::chrono::seconds(2)) {
			lastLog = now;
			OOVR_LOGF("DAPA MOTION v2: geometry=%d/%d interval=%.2fms horizon=%.2fms confidence=%.2f translation=(%.3f,%.3f,%.3f) turnRate=%.3f turnConfidence=%.2f appliedYaw=%.5f objectMV=off",
			    m_motionGeometryValid[0], m_motionGeometryValid[1], m_motion.interval*1000,
			    double(m_warpDisplayTime-m_motion.time)*1e-6, m_motion.confidence,
			    cb.poseDeltaMatrix[3], cb.poseDeltaMatrix[7], cb.poseDeltaMatrix[11],
			    m_turn.rate, m_turn.confidence, cb.padding[1]);
		}
	}

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	memcpy(mapped.pData, &cb, sizeof(cb));
	ctx->Unmap(m_constantBuffer, 0);

	BlackoutConstants blackoutCB{};
	const auto& blackout = m_cachedBlackout[eye];
	if (blackout.Active()) {
		blackoutCB.center[0] = blackout.centers[eye][0];
		blackoutCB.center[1] = blackout.centers[eye][1];
		blackoutCB.inner = blackout.inner;
		blackoutCB.middle = blackout.middle;
		blackoutCB.scale = blackout.horizontalScale;
		blackoutCB.cutoff = static_cast<float>(ocu_foveation::BlackoutCutoff(blackout.mask, blackout.middle));
		blackoutCB.flags = (blackout.mask.middle ? 1u : 0u) |
		    (blackout.mask.outer ? 2u : 0u) | (blackout.mask.cutoff ? 4u : 0u);
		blackoutCB.enabled = 1;
	}
	if (memcmp(&blackoutCB, &m_uploadedBlackout, sizeof(blackoutCB)) != 0) {
		hr = ctx->Map(m_blackoutConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(hr)) return false;
		memcpy(mapped.pData, &blackoutCB, sizeof(blackoutCB));
		ctx->Unmap(m_blackoutConstantBuffer, 0);
		m_uploadedBlackout = blackoutCB;
	}

	// Capture variant adds clean colour and diagnostic UAVs only for the requested
	// stereo pair. Its normal output still contains the configured game tint.
	ID3D11ComputeShader* captureShader = m_capture.BeginEye(eye, ctx,
	    m_cachedColor[eye], m_cachedDepth[eye], reinterpret_cast<const float*>(&cb),
	    m_motion.time, m_warpDisplayTime, m_cachedSourceFlipV[eye],
	    reinterpret_cast<const float*>(&m_cachedPose[eye]), reinterpret_cast<const float*>(&newPose),
	    reinterpret_cast<const float*>(&m_cachedFov[eye]),m_bodyValid[eye]?m_bodyDepth[eye].Get():nullptr);
	DapaComputeState savedComputeState(ctx, captureShader != nullptr);
	ctx->CSSetShader(captureShader ? captureShader : m_warpCS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[] = { m_srvColor[eye], m_bodyValid[eye] ? m_bodySrv[eye].Get() : nullptr, m_srvDepth[eye] };
	ctx->CSSetShaderResources(0, 3, srvs);
	ID3D11UnorderedAccessView* uavs[] = { m_uavOutput[eye], captureShader ? m_capture.Clean(eye) : nullptr,
	    captureShader ? m_capture.Diagnostic(eye) : nullptr };
	const UINT uavCount = captureShader ? 3 : 1;
	ctx->CSSetUnorderedAccessViews(0, uavCount, uavs, nullptr);
	ID3D11Buffer* constants[] = { m_constantBuffer, m_blackoutConstantBuffer };
	ctx->CSSetConstantBuffers(0, 2, constants);
	ctx->CSSetSamplers(0, 1, &m_linearSampler);

	uint32_t groupsX = (m_eyeWidth + 7) / 8;
	uint32_t groupsY = (m_eyeHeight + 7) / 8;
	{
		auto gpuSample = MeasureGpu(ctx, eye == 0 ? DapaGpuTiming::Stage::WarpLeft : DapaGpuTiming::Stage::WarpRight);
		ctx->Dispatch(groupsX, groupsY, 1);
	}

	// Unbind to avoid hazards
	ID3D11ShaderResourceView* nullSRVs[3] = {};
	ID3D11UnorderedAccessView* nullUAVs[3] = {};
	ctx->CSSetShaderResources(0, 3, nullSRVs);
	ctx->CSSetUnorderedAccessViews(0, uavCount, nullUAVs, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);
	if (captureShader) m_capture.EndEye(eye, ctx);

	return true;
}

bool ASWProvider::SubmitWarpedOutput(ID3D11DeviceContext* ctx, double displayPeriodMs)
{
	m_depthSubmittedThisFrame = false;
	if (!ctx || !m_ready || m_paused || !m_hasCachedFrame) return false;

	const auto deadline = std::chrono::steady_clock::now() +
	    std::chrono::nanoseconds(DapaTiming::ImageWaitBudget(displayPeriodMs));
	auto remaining = [&]() -> XrDuration {
		return std::max<XrDuration>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(
		    deadline - std::chrono::steady_clock::now()).count());
	};
	XrResult res = m_outputLease.Wait(m_outputSwapchain, remaining(),
	    xrAcquireSwapchainImage, xrWaitSwapchainImage);
	if (res != XR_SUCCESS) {
		if (res != XR_TIMEOUT_EXPIRED) {
			OOVR_LOGF("ASW: Output ownership failed result=%d — disabling ASW for this session", (int)res);
			m_ready = false;
		}
		return false;
	}
	const uint32_t idx = m_outputLease.index;

	// Copy both warped eyes into stereo-combined swapchain (use acquired index!)
	if (idx >= m_outputSwapchainImages.size()) {
		static int s = 0;
		if (s++ < 5) OOVR_LOGF("ASW: Acquired idx %u out of range (have %zu)", idx, m_outputSwapchainImages.size());
		m_outputLease.Release(m_outputSwapchain, xrReleaseSwapchainImage);
		m_ready = false;
		return false;
	}
	ID3D11Texture2D* target = m_outputSwapchainImages[idx];
	// Copy warped output (translation-corrected) into stereo-combined swapchain
	{
		DapaPredicationState unpredicated(ctx);
		auto gpuSample = MeasureGpu(ctx, DapaGpuTiming::Stage::Output);
		ctx->CopySubresourceRegion(target, 0,
		    0, 0, 0, m_warpedOutput[0], 0, nullptr); // left eye at x=0
		ctx->CopySubresourceRegion(target, 0,
		    m_eyeWidth, 0, 0, m_warpedOutput[1], 0, nullptr); // right eye at x=eyeWidth
	}

	// Release only after a successful wait and queued copies; no forced GPU drain.
	if (m_outputLease.Release(m_outputSwapchain, xrReleaseSwapchainImage) != XR_SUCCESS) {
		OOVR_LOG("ASW: Output release failed — disabling ASW for this session");
		m_ready = false;
		return false;
	}

	// Submit depth swapchain (if available). Skipped when the depth cache runs
	// at a different resolution than the eye (external render scale) — the
	// swapchain copy needs matching sizes and stale depth is worse than none.
	if (m_depthSwapchain != XR_NULL_HANDLE && DepthLayerValid()) {
		if (m_depthLease.failure != XR_SUCCESS) return true;
		XrResult depthRes = m_depthLease.Wait(m_depthSwapchain, remaining(),
		    xrAcquireSwapchainImage, xrWaitSwapchainImage);
		if (depthRes == XR_SUCCESS) {
			const uint32_t depthIdx = m_depthLease.index;
			bool copied = false;
			if (depthIdx < m_depthSwapchainImages.size()) {
				ID3D11Texture2D* depthTarget = m_depthSwapchainImages[depthIdx];
				DapaPredicationState unpredicated(ctx);
				auto gpuSample = MeasureGpu(ctx, DapaGpuTiming::Stage::DepthOutput);
				copied = m_depthTransfer.Copy(ctx, depthTarget, m_cachedDepth[0], m_cachedDepth[1]);
			}
			depthRes = m_depthLease.Release(m_depthSwapchain, xrReleaseSwapchainImage);
			m_depthSubmittedThisFrame = depthRes == XR_SUCCESS && copied;
			if (!copied)
				OOVR_LOG_LIMITEDF(5000, "ASW: depth atlas transfer unavailable; submitting colour without runtime depth");
		}
		if (depthRes != XR_SUCCESS && depthRes != XR_TIMEOUT_EXPIRED) {
			m_depthLease.failure = depthRes;
			OOVR_LOGF("ASW: Depth ownership failed result=%d — runtime depth attachment unavailable", (int)depthRes);
		}
	}

	static int s = 0;
	if (s++ < 3)
		OOVR_LOG("ASW: Warped output submitted to swapchain");

	return true;
}

// ============================================================================
// Shutdown
// ============================================================================

void ASWProvider::Shutdown()
{
	m_gpuTiming.Reset();
	m_depthTransfer.Reset();
	m_outputLease = {};
	m_depthLease = {};
	m_depthSubmittedThisFrame = false;
	m_ready = false;
	InvalidateCachedFrame();

	if (m_depthSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = {};
	}
	m_depthSwapchainImages.clear();

	if (m_outputSwapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(m_outputSwapchain);
		m_outputSwapchain = {};
	}
	m_outputSwapchainImages.clear();

	for (int i = 0; i < 2; i++) {
		if (m_uavOutput[i]) { m_uavOutput[i]->Release(); m_uavOutput[i] = nullptr; }
		if (m_warpedOutput[i]) { m_warpedOutput[i]->Release(); m_warpedOutput[i] = nullptr; }
		if (m_srvDepth[i]) { m_srvDepth[i]->Release(); m_srvDepth[i] = nullptr; }
		if (m_srvMV[i]) { m_srvMV[i]->Release(); m_srvMV[i] = nullptr; }
		if (m_srvColor[i]) { m_srvColor[i]->Release(); m_srvColor[i] = nullptr; }
		if (m_cachedDepth[i]) { m_cachedDepth[i]->Release(); m_cachedDepth[i] = nullptr; }
		if (m_cachedMV[i]) { m_cachedMV[i]->Release(); m_cachedMV[i] = nullptr; }
		if (m_cachedColor[i]) { m_cachedColor[i]->Release(); m_cachedColor[i] = nullptr; }
	}

	if (m_linearSampler) { m_linearSampler->Release(); m_linearSampler = nullptr; }
	for(int eye=0;eye<2;++eye){m_bodySrv[eye].Reset();m_bodyDepth[eye].Reset();m_bodyValid[eye]=false;}
	if (m_constantBuffer) { m_constantBuffer->Release(); m_constantBuffer = nullptr; }
	if (m_blackoutConstantBuffer) { m_blackoutConstantBuffer->Release(); m_blackoutConstantBuffer = nullptr; }
	m_uploadedBlackout = {};
	if (m_warpCS) { m_warpCS->Release(); m_warpCS = nullptr; }
	if (m_device) { m_device->Release(); m_device = nullptr; }

	m_eyeWidth = 0;
	m_eyeHeight = 0;
	OOVR_LOG("ASW: Shutdown");
}
