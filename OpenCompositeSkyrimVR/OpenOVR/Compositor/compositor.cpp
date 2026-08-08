#include "stdafx.h"

#include "compositor.h"

#include "../OpenCompositeInterface.h"

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

// The exported C ABI lives here, not in a file of its own: a dllexport in a static library only
// reaches the DLL if the linker pulls its object file in, and this one is pulled in
// unconditionally because every compositor derives from Compositor.

static const OpenCompositeInterface g_interface = {
	sizeof(OpenCompositeInterface),
	OPENCOMPOSITE_INTERFACE_VERSION,
	&Compositor::InvalidateSwapchains,
};

extern "C" __declspec(dllexport) const OpenCompositeInterface* OpenComposite_GetInterface(
    uint32_t minimumVersion)
{
	// A consumer built against a newer header than this build is asking for something we cannot
	// promise the layout of. Saying no is the only honest answer, and it degrades cleanly.
	if (minimumVersion > OPENCOMPOSITE_INTERFACE_VERSION)
		return nullptr;
	return &g_interface;
}
