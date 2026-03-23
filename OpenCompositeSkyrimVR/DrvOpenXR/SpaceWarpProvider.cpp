#include "SpaceWarpProvider.h"

#include "../OpenOVR/Misc/xr_ext.h"
#include "../OpenOVR/logging.h"

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>

// Global: set during extension registration in DrvOpenXR.cpp
bool g_spaceWarpAvailable = false;
SpaceWarpProvider* g_spaceWarpProvider = nullptr;

SpaceWarpProvider::~SpaceWarpProvider()
{
	Shutdown();
}

bool SpaceWarpProvider::CreateSwapchain(uint32_t width, uint32_t height, int64_t format,
    XrSwapchain* outChain, ID3D11Texture2D** outImage)
{
	XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	XrResult res = xrCreateSwapchain(xr_session.get(), &ci, outChain);
	if (XR_FAILED(res)) {
		OOVR_LOGF("SpaceWarp: xrCreateSwapchain failed (fmt=%lld %ux%u) result=%d",
		    format, width, height, (int)res);
		return false;
	}

	// Enumerate images — we only need the first (single-buffered for our copy target)
	uint32_t imageCount = 0;
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(*outChain, 0, &imageCount, nullptr));
	if (imageCount == 0) {
		OOVR_LOG("SpaceWarp: xrEnumerateSwapchainImages returned 0 images");
		xrDestroySwapchain(*outChain);
		*outChain = XR_NULL_HANDLE;
		return false;
	}

	std::vector<XrSwapchainImageD3D11KHR> images(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_SOFT_ABORT(xrEnumerateSwapchainImages(*outChain,
	    imageCount, &imageCount, (XrSwapchainImageBaseHeader*)images.data()));

	// Cache the first image's DX11 texture pointer (used as CopySubresourceRegion target)
	*outImage = images[0].texture;

	OOVR_LOGF("SpaceWarp: Created swapchain %ux%u fmt=%lld imageCount=%u",
	    width, height, format, imageCount);
	return true;
}

void SpaceWarpProvider::DestroySwapchain(XrSwapchain* chain)
{
	if (*chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(*chain);
		*chain = XR_NULL_HANDLE;
	}
}

bool SpaceWarpProvider::Initialize(uint32_t perEyeWidth, uint32_t perEyeHeight)
{
	if (m_ready) return true;
	if (perEyeWidth == 0 || perEyeHeight == 0) return false;

	OOVR_LOGF("SpaceWarp: Initializing — per-eye %ux%u, stereo-combined %ux%u",
	    perEyeWidth, perEyeHeight, perEyeWidth * 2, perEyeHeight);

	m_perEyeWidth = perEyeWidth;
	m_perEyeHeight = perEyeHeight;

	uint32_t stereoWidth = perEyeWidth * 2;

	// Create stereo-combined MV swapchain (R16G16_FLOAT — matches game's kMOTION_VECTOR)
	if (!CreateSwapchain(stereoWidth, perEyeHeight, DXGI_FORMAT_R16G16_FLOAT,
	        &m_mvSwapchain, &m_mvImage)) {
		OOVR_LOG("SpaceWarp: Failed to create stereo MV swapchain");
		Shutdown();
		return false;
	}

	// Create stereo-combined depth swapchain (R32_FLOAT)
	if (!CreateSwapchain(stereoWidth, perEyeHeight, DXGI_FORMAT_R32_FLOAT,
	        &m_depthSwapchain, &m_depthImage)) {
		OOVR_LOG("SpaceWarp: Failed to create stereo depth swapchain");
		Shutdown();
		return false;
	}

	// Pre-fill static parts of the info structs
	for (int eye = 0; eye < 2; eye++) {
		m_info[eye] = {};
		m_info[eye].type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
		m_info[eye].next = nullptr;
		m_info[eye].layerFlags = 0;

		// MV sub-image: per-eye region within stereo-combined swapchain
		m_info[eye].motionVectorSubImage.swapchain = m_mvSwapchain;
		m_info[eye].motionVectorSubImage.imageRect.offset = { (int32_t)(eye * perEyeWidth), 0 };
		m_info[eye].motionVectorSubImage.imageRect.extent = { (int32_t)perEyeWidth, (int32_t)perEyeHeight };
		m_info[eye].motionVectorSubImage.imageArrayIndex = 0;

		// Depth sub-image: per-eye region within stereo-combined swapchain
		m_info[eye].depthSubImage.swapchain = m_depthSwapchain;
		m_info[eye].depthSubImage.imageRect.offset = { (int32_t)(eye * perEyeWidth), 0 };
		m_info[eye].depthSubImage.imageRect.extent = { (int32_t)perEyeWidth, (int32_t)perEyeHeight };
		m_info[eye].depthSubImage.imageArrayIndex = 0;

		// Identity pose delta — reference space doesn't change between frames
		m_info[eye].appSpaceDeltaPose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
		m_info[eye].appSpaceDeltaPose.position = { 0.0f, 0.0f, 0.0f };

		m_info[eye].minDepth = 0.0f;
		m_info[eye].maxDepth = 1.0f;
	}

	m_ready = true;
	m_hasSmoothedValues = false;
	OOVR_LOGF("SpaceWarp: Initialized — 2 stereo-combined swapchains (%ux%u)",
	    stereoWidth, perEyeHeight);
	return true;
}

void SpaceWarpProvider::Shutdown()
{
	m_ready = false;
	DestroySwapchain(&m_mvSwapchain);
	DestroySwapchain(&m_depthSwapchain);
	m_mvImage = nullptr;
	m_depthImage = nullptr;
	m_perEyeWidth = 0;
	m_perEyeHeight = 0;
	m_hasSmoothedValues = false;
	OOVR_LOG("SpaceWarp: Shutdown");
}

bool SpaceWarpProvider::SubmitFrame(ID3D11DeviceContext* ctx,
    ID3D11Texture2D* mvTex, const D3D11_BOX mvRegions[2],
    ID3D11Texture2D* depthTex, const D3D11_BOX depthRegions[2],
    float nearZ, float farZ)
{
	if (!m_ready) return false;

	// --- Smooth nearZ/farZ to prevent frame-to-frame depth judder ---
	// Exponential moving average: 90% old + 10% new. Prevents sudden jumps
	// in depth reprojection while still tracking genuine changes.
	constexpr float kSmooth = 0.1f;
	if (!m_hasSmoothedValues) {
		m_smoothNear = nearZ;
		m_smoothFar = farZ;
		m_hasSmoothedValues = true;
	} else {
		m_smoothNear += kSmooth * (nearZ - m_smoothNear);
		m_smoothFar += kSmooth * (farZ - m_smoothFar);
	}

	// --- MV swapchain: acquire once, copy both eyes, flush once, release ---
	{
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		XrResult res = xrAcquireSwapchainImage(m_mvSwapchain, &acquireInfo, &idx);
		if (XR_FAILED(res)) {
			{ static int s = 0; if (s++ < 5)
				OOVR_LOGF("SpaceWarp: MV acquire failed result=%d", (int)res);
			}
			return false;
		}

		XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = 100000000; // 100ms
		res = xrWaitSwapchainImage(m_mvSwapchain, &waitInfo);
		if (XR_FAILED(res)) {
			{ static int s = 0; if (s++ < 5)
				OOVR_LOGF("SpaceWarp: MV wait failed result=%d", (int)res);
			}
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(m_mvSwapchain, &rel);
			return false;
		}

		// Copy both eyes into stereo-combined swapchain (left at x=0, right at x=perEyeWidth)
		ctx->CopySubresourceRegion(m_mvImage, 0, 0, 0, 0, mvTex, 0, &mvRegions[0]);
		ctx->CopySubresourceRegion(m_mvImage, 0, m_perEyeWidth, 0, 0, mvTex, 0, &mvRegions[1]);

		ctx->Flush(); // Submit both copies before releasing to runtime
		XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(m_mvSwapchain, &relInfo);
	}

	// --- Depth swapchain: acquire once, copy both eyes, flush once, release ---
	bool hasDepth = (depthTex != nullptr);
	if (hasDepth) {
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		XrResult res = xrAcquireSwapchainImage(m_depthSwapchain, &acquireInfo, &idx);
		if (XR_FAILED(res)) {
			{ static int s = 0; if (s++ < 5)
				OOVR_LOGF("SpaceWarp: depth acquire failed result=%d", (int)res);
			}
			hasDepth = false;
		} else {
			XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			waitInfo.timeout = 100000000; // 100ms
			res = xrWaitSwapchainImage(m_depthSwapchain, &waitInfo);
			if (XR_FAILED(res)) {
				{ static int s = 0; if (s++ < 5)
					OOVR_LOGF("SpaceWarp: depth wait failed result=%d", (int)res);
				}
				XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &rel);
				hasDepth = false;
			} else {
				// Copy both eyes into stereo-combined depth swapchain
				ctx->CopySubresourceRegion(m_depthImage, 0, 0, 0, 0, depthTex, 0, &depthRegions[0]);
				ctx->CopySubresourceRegion(m_depthImage, 0, m_perEyeWidth, 0, 0, depthTex, 0, &depthRegions[1]);

				ctx->Flush();
				XrSwapchainImageReleaseInfo relInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(m_depthSwapchain, &relInfo);
			}
		}
	}

	// Update per-frame fields in both eye info structs
	for (int eye = 0; eye < 2; eye++) {
		m_info[eye].nearZ = m_smoothNear;
		m_info[eye].farZ = m_smoothFar;

		if (!hasDepth) {
			m_info[eye].depthSubImage.swapchain = XR_NULL_HANDLE;
		} else {
			m_info[eye].depthSubImage.swapchain = m_depthSwapchain;
		}
	}

	{ static int s = 0; if (s++ < 3)
		OOVR_LOGF("SpaceWarp: SubmitFrame near=%.2f(smooth=%.2f) far=%.1f(smooth=%.1f) depth=%s",
		    nearZ, m_smoothNear, farZ, m_smoothFar, hasDepth ? "yes" : "no");
	}

	return true;
}
