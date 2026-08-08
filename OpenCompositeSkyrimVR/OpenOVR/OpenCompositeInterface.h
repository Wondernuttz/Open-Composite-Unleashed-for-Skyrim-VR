#ifndef OPENCOMPOSITE_INTERFACE_H
#define OPENCOMPOSITE_INTERFACE_H

/*
 * A small, stable C ABI that OpenComposite exposes to code sitting underneath it - API layers,
 * injectors, upscalers. Every entry is something OpenComposite already does for its own reasons;
 * this only lets it be asked for, so nothing here is specific to any one consumer.
 *
 * Resolve it with GetProcAddress on whichever module implements OpenVR - usually openvr_api.dll.
 * Deliberately not an OpenXR extension or a chained struct: it has to be answerable without an
 * OpenXR instance and without either side having agreed on anything beforehand.
 *
 * Compatibility. This header is meant to be *copied* into a consumer, so the two sides compile
 * separately and will drift. Three rules make that safe: fields are only ever appended;
 * `structSize` is what the provider compiled, so use OPENCOMPOSITE_HAS before reading one; and a
 * consumer asks for the lowest version it can work with, not the highest it knows - asking for the
 * highest would make a newer consumer refuse a perfectly usable older provider. That covers every
 * direction of skew:
 *
 *   provider older, no export at all   GetProcAddress fails         consumer degrades
 *   provider older, fewer functions    structSize is smaller        OPENCOMPOSITE_HAS is false
 *   provider newer, more functions     structSize is larger         extra fields ignored
 *   provider cannot meet the request   returns NULL                 consumer degrades
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bumped when a field is appended. Consumers ask for the lowest version they need. */
#define OPENCOMPOSITE_INTERFACE_VERSION 1

/** The exported symbol. Undecorated, cdecl, x64. */
#define OPENCOMPOSITE_GET_INTERFACE_NAME "OpenComposite_GetInterface"

typedef struct OpenCompositeInterface {
	/** sizeof(OpenCompositeInterface) as the *provider* compiled it. Never trust your own. */
	uint32_t structSize;

	/** OPENCOMPOSITE_INTERFACE_VERSION as the provider compiled it. */
	uint32_t version;

	/**
	 * Discard swapchains and build fresh ones on the next submitted frame - the same path
	 * OpenComposite takes when the game changes render target size, pipeline flush included.
	 *
	 * The reason to ask is that the images an OpenXR swapchain hands out are fixed for its
	 * lifetime, so the only way to give an application different ones is to make it ask again.
	 *
	 * IMPORTANT - this does not reach every swapchain OpenComposite owns. The game eye chains,
	 * overlays, and the ASW and space warp providers rebuild; the keyboard, the menu laser and the
	 * overlay trail chain do not. Do not hand those chains images you will later need back.
	 *
	 * Takes effect on the next frame, not immediately. Safe from any thread and at any point after
	 * the OpenVR session exists, including from inside an OpenXR call OpenComposite is making.
	 * Calling it when nothing needs rebuilding costs one swapchain allocation and is not an error.
	 *
	 * Added in version 1.
	 */
	void (*InvalidateSwapchains)(void);
} OpenCompositeInterface;

/**
 * Fetch the interface, or NULL if the provider cannot offer at least `minimumVersion`.
 *
 * Pass the lowest version containing everything you need - 1 if you only want
 * InvalidateSwapchains - and use OPENCOMPOSITE_HAS for anything added later.
 */
typedef const OpenCompositeInterface*(*PFN_OpenComposite_GetInterface)(uint32_t minimumVersion);

/**
 * True when `field` is present in the struct the provider handed over *and* is non-null. Both
 * halves matter: an older provider returns a smaller struct, so reading the field at all would run
 * off the end of it, and a provider may publish it as null when the capability is compiled out.
 */
#define OPENCOMPOSITE_HAS(iface, field)                                                     \
	((iface) != NULL                                                                        \
	    && (iface)->structSize >= (uint32_t)(offsetof(OpenCompositeInterface, field)         \
	           + sizeof(((const OpenCompositeInterface*)0)->field))                          \
	    && (iface)->field != NULL)

#ifdef __cplusplus
}
#endif

#endif // OPENCOMPOSITE_INTERFACE_H
