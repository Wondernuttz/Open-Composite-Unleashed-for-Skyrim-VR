#include "ASWProvider.h"

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
    float _pad0, _pad1, _pad2; // pad to 16-byte boundary
};

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= (uint)resolution.x || tid.y >= (uint)resolution.y)
        return;

    // Output pixel UV in the NEW (warped) view
    float2 uv = ((float2)tid.xy + 0.5) / resolution;

    // 1. Read depth from old frame (approximate — depth changes slowly between frames)
    float d = depthTex[tid.xy];
    float denom = farZ - d * (farZ - nearZ);
    float linearDepth = (abs(denom) > 0.0001) ? (nearZ * farZ / denom) : farZ;
    linearDepth *= depthScale;  // adjust parallax intensity

    // 2. Reconstruct view-space position of this output pixel in NEW view
    float tanX = lerp(fovTanLeft, fovTanRight, uv.x);
    float tanY = lerp(fovTanUp,   fovTanDown,  uv.y);
    float3 newViewPos = float3(tanX * linearDepth, tanY * linearDepth, linearDepth);

    // 3. Transform from NEW view space to OLD view space (backward warping)
    float4 transformed = mul(poseDeltaMatrix, float4(newViewPos, 1.0));
    float3 oldViewPos = transformed.xyz;

    // 4. Project into OLD view UV to find where to sample from the cached frame
    float2 sourceUV = uv;  // fallback if behind camera
    if (oldViewPos.z > 0.001) {
        float oldTanX = oldViewPos.x / oldViewPos.z;
        float oldTanY = oldViewPos.y / oldViewPos.z;
        sourceUV.x = (oldTanX - fovTanLeft) / (fovTanRight - fovTanLeft);
        sourceUV.y = (oldTanY - fovTanUp) / (fovTanDown - fovTanUp);
    }

    // 5. Sample previous frame with bilinear filtering, clamp to edge
    sourceUV = saturate(sourceUV);
    float4 color = prevColor.SampleLevel(linearClamp, sourceUV, 0);

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
	m_hasCachedFrame = false;
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
	// Pad to 16-byte alignment (WarpConstants is 112 bytes, already aligned)
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

	OOVR_LOG("ASW: Staging textures created (2 eyes × 4 textures)");
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

void ASWProvider::CacheFrame(int eye, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
    const XrPosef& eyePose, const XrFovf& eyeFov,
    float nearZ, float farZ)
{
	if (!m_ready || eye < 0 || eye > 1) return;

	// Copy color (game eye texture → cached)
	if (colorTex && colorRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedColor[eye], 0, 0, 0, 0,
		    colorTex, 0, colorRegion)) {
			OOVR_LOG("ASW: TOCTOU — color texture freed during copy");
			return;
		}
	}

	// Copy motion vectors (bridge MV → cached)
	if (mvTex && mvRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedMV[eye], 0, 0, 0, 0,
		    mvTex, 0, mvRegion)) {
			OOVR_LOG("ASW: TOCTOU — MV texture freed during copy");
			return;
		}
	}

	// Copy depth (bridge depth → cached)
	if (depthTex && depthRegion) {
		if (!SafeBridgeCopy(ctx, m_cachedDepth[eye], 0, 0, 0, 0,
		    depthTex, 0, depthRegion)) {
			OOVR_LOG("ASW: TOCTOU — depth texture freed during copy");
			return;
		}
	}

	m_cachedPose[eye] = eyePose;
	m_cachedFov[eye] = eyeFov;
	m_cachedNear = nearZ;
	m_cachedFar = farZ;

	// Both eyes cached → ready for warping
	if (eye == 1) {
		m_hasCachedFrame = true;
		static int s = 0;
		if (s++ < 3)
			OOVR_LOGF("ASW: Frame cached — near=%.2f far=%.1f", nearZ, farZ);
	}
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

	// Scale translation part by master strength × translation scale
	float master = oovr_global_configuration.ASWWarpStrength();
	float transS = master * oovr_global_configuration.ASWTranslationScale();
	transS = (transS < 0.0f) ? 0.0f : transS;
	if (transS != 1.0f) {
		cb.poseDeltaMatrix[3] *= transS;
		cb.poseDeltaMatrix[7] *= transS;
		cb.poseDeltaMatrix[11] *= transS;
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

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return false;
	memcpy(mapped.pData, &cb, sizeof(cb));
	ctx->Unmap(m_constantBuffer, 0);

	// Dispatch compute shader
	ctx->CSSetShader(m_warpCS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[] = { m_srvColor[eye], m_srvMV[eye], m_srvDepth[eye] };
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

bool ASWProvider::SubmitWarpedOutput(ID3D11DeviceContext* ctx)
{
	if (!m_ready || !m_hasCachedFrame) return false;

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

	// Submit depth swapchain (if available)
	if (m_depthSwapchain != XR_NULL_HANDLE) {
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
	m_hasCachedFrame = false;

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
