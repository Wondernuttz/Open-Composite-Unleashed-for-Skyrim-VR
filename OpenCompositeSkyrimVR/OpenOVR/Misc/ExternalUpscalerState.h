#pragma once

#include <stdint.h>

#define OCU_EXTERNAL_UPSCALER_STATE_NAME L"Local\\OpenCompositeUnleashedUpscalingState"
#define OCU_EXTERNAL_UPSCALER_STATE_MAGIC 0x4F435553u
#define OCU_EXTERNAL_UPSCALER_STATE_VERSION 1u

enum OCUExternalUpscalerMethod : uint32_t {
	OCU_EXTERNAL_UPSCALER_NONE = 0,
	OCU_EXTERNAL_UPSCALER_DLSS = 1,
	OCU_EXTERNAL_UPSCALER_FSR3 = 2,
	OCU_EXTERNAL_UPSCALER_FSR_NATIVE_AA = 3,
	OCU_EXTERNAL_UPSCALER_DLAA = 4,
};

enum OCUExternalUpscalerFlags : uint32_t {
	OCU_EXTERNAL_UPSCALER_FLAG_DLSS = 1u << 0,
	OCU_EXTERNAL_UPSCALER_FLAG_FSR3 = 1u << 1,
	OCU_EXTERNAL_UPSCALER_FLAG_FSR_NATIVE_AA = 1u << 2,
	OCU_EXTERNAL_UPSCALER_FLAG_DLAA = 1u << 3,
	// OCU's internal ASW (synthetic frame generation) is enabled. External
	// render-scale systems (e.g. Community Shaders VR) should treat this as
	// "ASW may engage at any time": ASW warps the submitted frame using the
	// game's MV + depth render targets, so recreating those targets at a
	// different resolution mid-session breaks the warp (flashing).
	// NOTE for readers: check this flag even when `active` == 0 — ASW can be
	// on while OCU's own upscalers are off.
	OCU_EXTERNAL_UPSCALER_FLAG_ASW_ENABLED = 1u << 4,
};

struct OCUExternalUpscalerState {
	uint32_t magic;
	uint32_t version;
	uint32_t byteSize;
	uint32_t updateCounter;

	uint32_t active;
	uint32_t method;
	uint32_t dlssPreset;
	uint32_t flags;

	float renderScale;
	float mipBias;
	float mipBiasOffset;
	float reservedFloat0;

	uint32_t reserved[8];
};

void PublishExternalUpscalerState(
    bool active,
    OCUExternalUpscalerMethod method,
    float renderScale,
    float mipBias,
    float mipBiasOffset,
    uint32_t dlssPreset,
    uint32_t flags);
void ShutdownExternalUpscalerState();
