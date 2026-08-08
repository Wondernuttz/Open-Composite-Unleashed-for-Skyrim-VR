#include "stdafx.h"

#include "compositor.h"

#include <atomic>

Compositor::~Compositor()
{
	if (chain) {
		OOVR_FAILED_XR_SOFT_ABORT(xrDestroySwapchain(chain));
		chain = XR_NULL_HANDLE;
	}
}

// See compositor.h for why this is a counter and not a flag.

namespace {
std::atomic<uint32_t> g_swapchainGeneration{ 0 };
}

void Compositor::InvalidateSwapchains()
{
	const uint32_t now = g_swapchainGeneration.fetch_add(1) + 1;
	OOVR_LOGF("Swapchain invalidation requested - generation %u. Every compositor will rebuild on "
	          "its next submitted frame.",
	    now);
}

uint32_t Compositor::SwapchainGeneration()
{
	return g_swapchainGeneration.load();
}

// The OCU_InvalidateSwapchains export that reaches this lives in OCOVR/openvr_api.cpp, next to the
// other OCU_ exports - it has to be in the DLL's own sources, not in a static library.
