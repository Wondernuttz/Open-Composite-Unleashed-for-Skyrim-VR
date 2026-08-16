#include "ASWProvider.h"

#include "../OpenOVR/Compositor/compositor.h"
#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/logging.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

// Global instance — accessed from XrBackend for frame injection
ASWProvider* g_aswProvider = nullptr;

// ============================================================================
// Embedded HLSL compute shader for frame warping
// ============================================================================
static const char* s_warpShaderHLSL = R"(

Texture2D<float4> prevColor    : register(t0);
Texture2D<float2> motionVectors : register(t1);
Texture2D<float>  depthTex     : register(t2);
RWTexture2D<float4> output     : register(u0);
SamplerState linearClamp       : register(s0);

cbuffer WarpParams : register(b0) {
    row_major float4x4 poseDeltaMatrix;   // transforms NEW view → OLD view (backward warp)
    float2 resolution;
    float nearZ, farZ;
    float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
    float depthScale;           // multiplier on linearized depth (parallax intensity)
    float edgeFadeWidth;        // depth-edge fade threshold (depth ratio units)
    float nearFadeDepth;        // parallax fades to 0 below this depth (game units); 0 = disabled
    float debugTint;            // >0.5 = red-tint warp frames (aswDebugMode=10)
    float2 depthResolution;     // depth grid size — may be smaller than resolution
                                // when an external render-scale mod is active
    float2 sourceFlip;          // x is reserved; y preserves reversed-V OpenVR bounds
};

// Linearize depth from reversed-Z buffer value
float LinearizeDepth(float d, float zNear, float zFar) {
    float denom = zFar - d * (zFar - zNear);
    return (abs(denom) > 0.0001) ? (zNear * zFar / denom) : zFar;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    // Output pixel UV in the NEW (warped) view
    float2 uv = ((float2)tid.xy + 0.5) / resolution;

    // 1. Read depth from old frame (approximate — depth changes slowly between frames).
    // Depth may live at a different (smaller) grid than the color/output when an
    // external render-scale mod is active, so index it through UV, not tid.xy.
    // The bridge depth target is raster-aligned with the submitted game color.
    // Canonicalize it for warp sampling; the raw XR depth layer is disabled
    // for flipped pairs because that swapchain is not transformed here.
    float2 depthUV = lerp(uv, 1.0 - uv, sourceFlip);
    int2 dmax = int2((int)depthResolution.x - 1, (int)depthResolution.y - 1);
    int2 dpix = clamp((int2)(depthUV * depthResolution), int2(0,0), dmax);
    float d = depthTex[dpix];
    float linearDepth = LinearizeDepth(d, nearZ, farZ);

    // 2. Depth-edge detection: fade parallax at discontinuities to prevent silhouette tears
    float minD = linearDepth, maxD = linearDepth;
    int2 offsets[4] = { int2(-1,0), int2(1,0), int2(0,-1), int2(0,1) };
    [unroll] for (int i = 0; i < 4; i++) {
        int2 np = clamp(dpix + offsets[i], int2(0,0), dmax);
        float nd = LinearizeDepth(depthTex[np], nearZ, farZ);
        minD = min(minD, nd);
        maxD = max(maxD, nd);
    }
    float depthRatio = maxD / max(minD, 0.001);
    float edgeFade = saturate(1.0 - (depthRatio - 1.0) / max(edgeFadeWidth, 0.001));

    // 3. Reconstruct view-space position of this output pixel in NEW view
    float scaledDepth = linearDepth * depthScale;
    float tanX = lerp(fovTanLeft, fovTanRight, uv.x);
    float tanY = lerp(fovTanUp,   fovTanDown,  uv.y);
    float3 newViewPos = float3(tanX * scaledDepth, tanY * scaledDepth, scaledDepth);

    // 4. Transform from NEW view space to OLD view space (backward warping)
    float4 transformed = mul(poseDeltaMatrix, float4(newViewPos, 1.0));
    float3 oldViewPos = transformed.xyz;

    // 5. Project into OLD view UV to find where to sample from the cached frame
    float2 parallaxUV = uv;  // fallback if behind camera
    if (oldViewPos.z > 0.001) {
        float oldTanX = oldViewPos.x / oldViewPos.z;
        float oldTanY = oldViewPos.y / oldViewPos.z;
        parallaxUV.x = (oldTanX - fovTanLeft) / (fovTanRight - fovTanLeft);
        parallaxUV.y = (oldTanY - fovTanUp) / (fovTanDown - fovTanUp);
    }

    // 6. Near-field fade: zero parallax below nearFadeDepth, full at 2x (hands, close walls)
    float depthFade = (nearFadeDepth > 0.0) ? saturate((linearDepth - nearFadeDepth) / nearFadeDepth) : 1.0;

    // 7. Apply combined fade; OOB warp falls back to identity
    float2 sourceUV = lerp(uv, parallaxUV, edgeFade * depthFade);
    if (any(sourceUV < -0.01) || any(sourceUV > 1.01))
        sourceUV = uv;
    sourceUV = saturate(sourceUV);

    float2 colorUV = lerp(sourceUV, 1.0 - sourceUV, sourceFlip);
    float4 color = prevColor.SampleLevel(linearClamp, colorUV, 0);

    // Debug: tint warp frames red so they're distinguishable from real frames
    if (debugTint > 0.5) {
        color.rgb = float3(min(1.0, color.r * 1.5 + 0.1), color.g * 0.6, color.b * 0.6);
    }

    output[tid.xy] = color;
}
)";

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
	// Pad to 16-byte alignment (WarpConstants is 128 bytes, already aligned)
	cbDesc.ByteWidth = (cbDesc.ByteWidth + 15) & ~15;
	cbDesc.Usage = D3D11_USAGE_DYNAMIC;
	cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("ASW: CreateBuffer (CB) failed hr=0x%08x", (unsigned)hr);
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
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = DXGI_FORMAT_R32_FLOAT;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	XrResult res = xrCreateSwapchain(xr_session.get(), &ci, &m_depthSwapchain);
	if (XR_FAILED(res)) {
		OOVR_LOGF("ASW: xrCreateSwapchain (depth) failed (%ux%u) result=%d", width, height, (int)res);
		return false;
	}

	uint32_t imageCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_depthSwapchain, 0, &imageCount, nullptr));
	if (imageCount == 0) {
		OOVR_LOG("ASW: Depth swapchain has 0 images");
		xrDestroySwapchain(m_depthSwapchain);
		m_depthSwapchain = {};
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(m_depthSwapchain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()));

	m_depthSwapchainImages.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; i++)
		m_depthSwapchainImages[i] = images[i].texture;

	OOVR_LOGF("ASW: Depth swapchain created %ux%u (%u images)", width, height, imageCount);
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
    float nearZ, float farZ)
{
	auto failGeneration = [this]() {
		InvalidateCachedFrame();
		return false;
	};

	if (!m_ready || eye < 0 || eye > 1)
		return failGeneration();

	// A left-eye submit begins a new generation. The provider has a single
	// stereo cache, so the previously published pair must stop being visible
	// before either eye is overwritten.
	if (eye == 0) {
		InvalidateCachedFrame();
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

	// The XR depth layer needs depth at the eye size; the parallax warp does not.
	m_depthLayerValid = (m_depthWidth == m_eyeWidth && m_depthHeight == m_eyeHeight);

	// Copy color (game eye texture → cached)
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

	m_cachedPose[eye] = eyePose;
	m_cachedFov[eye] = eyeFov;
	m_cachedSourceFlipV[eye] = sourceFlipV;
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

bool ASWProvider::WarpFrame(int eye, ID3D11DeviceContext* ctx,
    const XrPosef& newPose)
{
	if (!m_ready || !m_hasCachedFrame || eye < 0 || eye > 1) return false;

	// Build pose delta matrix
	WarpConstants cb = {};
	// Backward warping: matrix transforms NEW view → OLD view
	// so we can find where each output pixel maps to in the cached frame
	BuildPoseDeltaMatrix(newPose, m_cachedPose[eye], cb.poseDeltaMatrix);

	// Rotation correction is DISABLED — VD's runtime ATW handles rotation.
	// We only apply translation (depth-based parallax) to correct for strafing/positional movement.
	// Force rotation part (3x3 upper-left) to identity:
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			cb.poseDeltaMatrix[r * 4 + c] = (r == c) ? 1.0f : 0.0f;

	// Stick turn correction: yaw delta is per game frame; warp sits ~half a frame after cache.
	// Default aswRotationScale=0 (off); start tuning at 0.5, negate if direction is backwards.
	float rotS = oovr_global_configuration.ASWRotationScale();
	if (rotS != 0.0f && m_locoYaw != 0.0f) {
		float theta = m_locoYaw * rotS;
		float c = cosf(theta), s = sinf(theta);
		// R_y(theta) row-major into the 3x3 block
		cb.poseDeltaMatrix[0] = c;
		cb.poseDeltaMatrix[2] = s;
		cb.poseDeltaMatrix[8] = -s;
		cb.poseDeltaMatrix[10] = c;
	}

	// Scale translation part by master strength × translation scale.
	// HMD pose delta is meters; shader view space is game units → ×72 (matches nearFadeDepth conversion).
	// Negated: field-tested — positive scale displaced the warp further along travel direction.
	float master = oovr_global_configuration.ASWWarpStrength();
	float transS = master * oovr_global_configuration.ASWTranslationScale() * 72.0f;
	transS = (transS < 0.0f) ? 0.0f : transS;
	cb.poseDeltaMatrix[3] *= -transS;
	cb.poseDeltaMatrix[7] *= -transS;
	cb.poseDeltaMatrix[11] *= -transS;

	// Stick locomotion correction: shift by the warp's position within the game frame
	// (slot fraction) × the per-frame view-space camera delta. Game units, matches linearDepth.
	// Negated: field-tested — positive sign doubled the travel-direction displacement.
	float locoS = m_slotFraction * oovr_global_configuration.ASWLocoScale();
	if (locoS != 0.0f && (m_locoX != 0.0f || m_locoY != 0.0f || m_locoZ != 0.0f)) {
		cb.poseDeltaMatrix[3] -= m_locoX * locoS;
		cb.poseDeltaMatrix[7] -= m_locoY * locoS;
		cb.poseDeltaMatrix[11] -= m_locoZ * locoS;
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

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	memcpy(mapped.pData, &cb, sizeof(cb));
	ctx->Unmap(m_constantBuffer, 0);

	// Dispatch compute shader
	ctx->CSSetShader(m_warpCS, nullptr, 0);
	// t1 null — MV texture is never copied or sampled (parallax-only shader)
	ID3D11ShaderResourceView* srvs[] = { m_srvColor[eye], nullptr, m_srvDepth[eye] };
	ctx->CSSetShaderResources(0, 3, srvs);
	ID3D11UnorderedAccessView* uavs[] = { m_uavOutput[eye] };
	ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	ctx->CSSetConstantBuffers(0, 1, &m_constantBuffer);
	ctx->CSSetSamplers(0, 1, &m_linearSampler);

	uint32_t groupsX = (m_eyeWidth + 7) / 8;
	uint32_t groupsY = (m_eyeHeight + 7) / 8;
	ctx->Dispatch(groupsX, groupsY, 1);

	// Unbind to avoid hazards
	ID3D11ShaderResourceView* nullSRVs[3] = {};
	ID3D11UnorderedAccessView* nullUAVs[1] = {};
	ctx->CSSetShaderResources(0, 3, nullSRVs);
	ctx->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	ctx->CSSetShader(nullptr, nullptr, 0);

	return true;
}

bool ASWProvider::RebuildSwapchainsIfInvalidated()
{
	const uint32_t current = Compositor::SwapchainGeneration();
	if (current == m_swapchainGeneration)
		return true;

	OOVR_LOGF("ASW: swapchain generation %u -> %u - rebuilding", m_swapchainGeneration, current);

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

	if (!CreateOutputSwapchain(m_eyeWidth * 2, m_eyeHeight)) {
		// The old chain is already gone, so there is nothing to fall back to. Stop rather than
		// leave m_ready set over a null handle we would retry against for the rest of the session.
		OOVR_LOG("ASW: output swapchain rebuild failed - disabling ASW");
		m_ready = false;
		return false;
	}
	if (!CreateDepthSwapchain(m_eyeWidth * 2, m_eyeHeight)) {
		// Same as at init: ASW works without depth, the runtime just loses the depth attachment.
		OOVR_LOG("ASW: depth swapchain rebuild failed (non-fatal - depth layer disabled)");
	}

	// Only once the chains actually exist, so a failure is retried rather than skipped.
	m_swapchainGeneration = current;
	return true;
}

bool ASWProvider::SubmitWarpedOutput(ID3D11DeviceContext* ctx)
{
	if (!m_ready || !m_hasCachedFrame) return false;

	if (!RebuildSwapchainsIfInvalidated())
		return false;

	// Acquire output swapchain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult res = xrAcquireSwapchainImage(m_outputSwapchain, &acquireInfo, &idx);
	if (XR_FAILED(res)) {
		static int s = 0;
		if (s++ < 5) OOVR_LOGF("ASW: Output acquire failed result=%d", (int)res);
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = XR_INFINITE_DURATION; // our own swapchain — runtime always returns images
	res = xrWaitSwapchainImage(m_outputSwapchain, &waitInfo);
	if (XR_FAILED(res)) {
		// Wait failed on our own swapchain — something is seriously wrong.
		// Do NOT release: spec says release after failed wait is XR_ERROR_CALL_ORDER_INVALID.
		// Image stays acquired — swapchain is now stuck. Disable ASW for this session.
		OOVR_LOGF("ASW: Output wait FAILED result=%d — disabling ASW (swapchain stuck)", (int)res);
		m_ready = false;
		return false;
	}

	// Copy both warped eyes into stereo-combined swapchain (use acquired index!)
	if (idx >= m_outputSwapchainImages.size()) {
		static int s = 0;
		if (s++ < 5) OOVR_LOGF("ASW: Acquired idx %u out of range (have %zu)", idx, m_outputSwapchainImages.size());
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_outputSwapchain, &rel);
		return false;
	}
	ID3D11Texture2D* target = m_outputSwapchainImages[idx];
	// Copy warped output (translation-corrected) into stereo-combined swapchain
	ctx->CopySubresourceRegion(target, 0,
	    0, 0, 0, m_warpedOutput[0], 0, nullptr); // left eye at x=0
	ctx->CopySubresourceRegion(target, 0,
	    m_eyeWidth, 0, 0, m_warpedOutput[1], 0, nullptr); // right eye at x=eyeWidth

	// No manual Flush() — xrReleaseSwapchainImage handles GPU synchronization
	XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(m_outputSwapchain, &relInfo);

	// Submit depth swapchain (if available). Skipped when the depth cache runs
	// at a different resolution than the eye (external render scale) — the
	// swapchain copy needs matching sizes and stale depth is worse than none.
	if (m_depthSwapchain != XR_NULL_HANDLE && DepthLayerValid()) {
		XrSwapchainImageAcquireInfo depthAcquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t depthIdx = 0;
		XrResult depthRes = xrAcquireSwapchainImage(m_depthSwapchain, &depthAcquire, &depthIdx);
		if (XR_SUCCEEDED(depthRes)) {
			XrSwapchainImageWaitInfo depthWait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			depthWait.timeout = XR_INFINITE_DURATION;
			depthRes = xrWaitSwapchainImage(m_depthSwapchain, &depthWait);
			if (XR_SUCCEEDED(depthRes)) {
				if (depthIdx < m_depthSwapchainImages.size()) {
					ID3D11Texture2D* depthTarget = m_depthSwapchainImages[depthIdx];
					D3D11_BOX depthBox = {};
					depthBox.right = m_eyeWidth;
					depthBox.bottom = m_eyeHeight;
					depthBox.front = 0;
					depthBox.back = 1;
					ctx->CopySubresourceRegion(depthTarget, 0,
					    0, 0, 0, m_cachedDepth[0], 0, &depthBox);
					ctx->CopySubresourceRegion(depthTarget, 0,
					    m_eyeWidth, 0, 0, m_cachedDepth[1], 0, &depthBox);
				}
				XrSwapchainImageReleaseInfo depthRel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &depthRel);
			} else {
				// Wait failed — don't release (spec violation). Depth swapchain stuck but non-fatal.
				OOVR_LOGF("ASW: Depth wait failed result=%d — depth layer disabled", (int)depthRes);
			}
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
	if (m_constantBuffer) { m_constantBuffer->Release(); m_constantBuffer = nullptr; }
	if (m_warpCS) { m_warpCS->Release(); m_warpCS = nullptr; }
	if (m_device) { m_device->Release(); m_device = nullptr; }

	m_eyeWidth = 0;
	m_eyeHeight = 0;
	OOVR_LOG("ASW: Shutdown");
}
