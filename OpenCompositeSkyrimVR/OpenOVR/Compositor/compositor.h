#pragma once

#include "generated/interfaces/vrtypes.h"
#ifdef WIN32
// Windows template libraries
#include <atlbase.h>
#include <wrl/client.h>
#endif

#include "../Misc/xr_ext.h"
#include "../Misc/xrutil.h"

#include <memory>
#include <vector>

#ifdef WIN32
using Microsoft::WRL::ComPtr;
#endif

typedef unsigned int GLuint;

class Compositor {
public:
	virtual ~Compositor();

	// Only copy a texture - this can be used for overlays and such
	virtual void Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds) = 0;

	virtual void Invoke(XruEye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
	    vr::EVRSubmitFlags submitFlags, XrCompositionLayerProjectionView& viewport)
	    = 0;

	virtual void InvokeCubemap(const vr::Texture_t* textures) = 0;
	virtual bool SupportsCubemap() { return false; }

	virtual XrSwapchain GetSwapChain() { return chain; };

	virtual XrExtent2Df GetSrcSize() { return { (float)createInfo.width, (float)createInfo.height }; }
	/**
	 * Loads and unloads some context required for submitting textures to LibOVR. LoadSubmitContext is
	 *  called before calling either Invoke or ovr_CommitTextureSwapChain, and ResetSubmitContext after
	 *  calling both of them.
	 */
	virtual void LoadSubmitContext() {};
	virtual void ResetSubmitContext() {};

	virtual void PreemptLeftEye() {};

	// When true, skip all post-processing (FSR/DLAA/CAS) and use direct copy.
	// Set for overlay compositors — post-processing is designed for game eye textures only.
	bool isOverlay = false;

	/**
	 * Throw away the swapchains of every participating owner and build fresh ones on the next
	 * submitted frame. The same thing we do on our own initiative when the game's submitted
	 * texture changes size or format, asked for from outside. Reached from a mod through the
	 * OCU_InvalidateSwapchains export; see docs/API-LAYERS.md.
	 *
	 * A counter rather than a flag, because the owners are independent and a consumed-once flag
	 * would be cleared by whichever submitted first, leaving the rest never rebuilding. Each
	 * compares against its own stored copy, so they need no coordination.
	 *
	 * Participating owners, exhaustively: DX11Compositor (game eyes and overlays), ASWProvider,
	 * SpaceWarpProvider. VRKeyboard, VRMenuLaser and BaseOverlay's trail chain call
	 * xrCreateSwapchain directly and do NOT rebuild — a caller must not hand those chains images
	 * it will later need back, because asking will not get them back.
	 *
	 * Takes effect on the next frame. Safe from any thread.
	 */
	static void InvalidateSwapchains();

	/** The current value. A compositor rebuilds when this stops matching what it built against. */
	static uint32_t SwapchainGeneration();

protected:
	XrSwapchain chain = XR_NULL_HANDLE;

	// What SwapchainGeneration() read when the current chain was built. Zero unless something has
	// asked for a rebuild, so a compositor that never reads it behaves exactly as it always has.
	uint32_t swapchainGeneration = 0;

	// The request used to create the current swapchain. This can be used to check if the swapchain needs recreating.
	XrSwapchainCreateInfo createInfo{};

	// The format specified by the game when creating the swapchain. This is used for verifying the format hasn't changed, since
	// we do fiddle with it a bit to get the SRGB stuff done correctly.
	int64_t createInfoFormat;
};
