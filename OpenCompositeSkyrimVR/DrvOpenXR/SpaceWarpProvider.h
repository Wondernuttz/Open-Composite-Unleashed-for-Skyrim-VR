#pragma once

#include "XrDriverPrivate.h"
#include <d3d11.h>

// ============================================================================
// Local XR_FB_space_warp type definitions
// Bundled OpenXR SDK is v1.0.12; XR_FB_space_warp was added in v1.0.22.
// These are exact copies of the Khronos spec definitions.
// ============================================================================

#ifndef XR_FB_space_warp
#define XR_FB_space_warp 1
#define XR_FB_space_warp_SPEC_VERSION 2

#define XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB  ((XrStructureType)1000171000)
#define XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB       ((XrStructureType)1000171001)

typedef struct XrCompositionLayerSpaceWarpInfoFB {
	XrStructureType             type;
	const void*                 next;
	XrCompositionLayerFlags     layerFlags;
	XrSwapchainSubImage         motionVectorSubImage;
	XrPosef                     appSpaceDeltaPose;
	XrSwapchainSubImage         depthSubImage;
	float                       minDepth;
	float                       maxDepth;
	float                       nearZ;
	float                       farZ;
} XrCompositionLayerSpaceWarpInfoFB;

typedef struct XrSystemSpaceWarpPropertiesFB {
	XrStructureType type;
	void*           next;
	uint32_t        recommendedMotionVectorImageRectWidth;
	uint32_t        recommendedMotionVectorImageRectHeight;
} XrSystemSpaceWarpPropertiesFB;

#endif // XR_FB_space_warp

// Global: set to true during extension registration if runtime supports it
extern bool g_spaceWarpAvailable;

class SpaceWarpProvider;
extern SpaceWarpProvider* g_spaceWarpProvider;

class SpaceWarpProvider {
public:
	SpaceWarpProvider() = default;
	~SpaceWarpProvider();

	SpaceWarpProvider(const SpaceWarpProvider&) = delete;
	SpaceWarpProvider& operator=(const SpaceWarpProvider&) = delete;

	/// Initialize: create stereo-combined XR swapchains for MV + depth.
	/// @param perEyeWidth  Per-eye motion vector width  (bridge MV texture width / 2)
	/// @param perEyeHeight Per-eye motion vector height
	bool Initialize(uint32_t perEyeWidth, uint32_t perEyeHeight);
	void Shutdown();
	bool IsReady() const { return m_ready; }

	/// Submit both eyes' data in a single call. Acquires each swapchain once,
	/// copies both eyes' regions, flushes once, releases.
	/// Call AFTER both eyes' textures are available (after right eye submit).
	bool SubmitFrame(ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* mvTex, const D3D11_BOX mvRegions[2],
	    ID3D11Texture2D* depthTex, const D3D11_BOX depthRegions[2],
	    float nearZ, float farZ);

	/// Returns pointer to the filled struct for the given eye.
	XrCompositionLayerSpaceWarpInfoFB* GetLayerInfo(int eye) { return &m_info[eye]; }

private:
	bool CreateSwapchain(uint32_t width, uint32_t height, int64_t format,
	    XrSwapchain* outChain, ID3D11Texture2D** outImage);
	void DestroySwapchain(XrSwapchain* chain);

	bool m_ready = false;
	uint32_t m_perEyeWidth = 0, m_perEyeHeight = 0;

	// Stereo-combined XR swapchains (both eyes side-by-side, width = perEye * 2)
	XrSwapchain m_mvSwapchain = {};
	XrSwapchain m_depthSwapchain = {};

	// Cached DX11 image pointers from swapchain enumeration
	ID3D11Texture2D* m_mvImage = nullptr;
	ID3D11Texture2D* m_depthImage = nullptr;

	// Pre-filled info structs (one per eye, persists across frame)
	XrCompositionLayerSpaceWarpInfoFB m_info[2] = {};

	// Smoothed nearZ/farZ to reduce frame-to-frame judder
	float m_smoothNear = 0.0f;
	float m_smoothFar = 0.0f;
	bool m_hasSmoothedValues = false;
};
