#pragma once

#include <d3d11.h>

#include <memory>

#include "Misc/xr_ext.h"
#include "Misc/xrutil.h"

// The menu, console, and keyboard lasers all use the same four immutable
// sprites. Keep them in one OpenXR swapchain and select the required sprite
// with XrSwapchainSubImage::imageRect instead of spending one swapchain per
// sprite (or per laser consumer).
enum class LaserAtlasRegion {
	BeamIdle = 0,
	BeamClicked,
	DotIdle,
	DotClicked,
};

class LaserTextureAtlas {
public:
	// Returns the atlas already alive for this OpenXR session, or creates it.
	// The resource is reference-counted so overlapping menu/console/keyboard
	// consumers share the exact same swapchain safely.
	static std::shared_ptr<LaserTextureAtlas> Acquire(ID3D11Device* dev);

	~LaserTextureAtlas();

	LaserTextureAtlas(const LaserTextureAtlas&) = delete;
	LaserTextureAtlas& operator=(const LaserTextureAtlas&) = delete;

	XrSwapchain GetSwapchain() const { return chain; }
	XrRect2Di GetImageRect(LaserAtlasRegion region) const;
	DXGI_FORMAT GetFormat() const { return format; }
	XrSession GetSession() const { return session; }

private:
	LaserTextureAtlas(ID3D11Device* dev, XrSession session);

	XrSession session = XR_NULL_HANDLE;
	XrSwapchain chain = XR_NULL_HANDLE;
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};
