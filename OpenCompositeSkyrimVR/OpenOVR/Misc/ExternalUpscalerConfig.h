#pragma once

#include "ExternalUpscalerState.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ExternalUpscalerConfig {
inline constexpr float kTemporalRenderScaleThreshold = 0.99f;

struct Input {
	bool fsr3Available = false;
	bool dlssAvailable = false;

	bool fsrEnabled = false;
	bool fsrNativeAA = false;
	bool dlssEnabled = false;
	bool dlaaEnabled = false;
	bool aswEnabled = false;
	float renderScale = 1.0f; // FSR render scale (fsrRenderScale)
	// DLSS render scale resolved by the caller (override if set, otherwise the
	// preset's scale). MUST NOT be conflated with the FSR scale: gating DLSS on
	// fsrRenderScale published active=0 / wrong scale for DLSS users, which
	// broke Community Shaders' mip bias (fixed 2026-07-16, regressed in the
	// original version of this module, re-fixed at merge).
	float dlssRenderScale = 1.0f;
	int dlssPreset = 0;

	bool mipBiasEnabled = false;
	std::string_view mipBiasMode;
	float mipBiasOffset = 0.0f;
	float fsr3MipBiasOffset = 0.0f;
	float dlssMipBiasOffset = 0.0f;
};

struct State {
	bool active = false;
	OCUExternalUpscalerMethod method = OCU_EXTERNAL_UPSCALER_NONE;
	float renderScale = 1.0f;
	float mipBias = 0.0f;
	float mipBiasOffset = 0.0f;
	uint32_t dlssPreset = 0;
	uint32_t flags = 0;
};

namespace Detail {
inline std::string NormalizeMipBiasMode(std::string_view value)
{
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
		value.remove_prefix(1);
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
		value.remove_suffix(1);

	std::string normalized(value);
	std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return normalized;
}

inline bool TryParseFiniteFloat(const std::string& value, float& result)
{
	errno = 0;
	char* end = nullptr;
	const char* begin = value.c_str();
	const float parsed = std::strtof(begin, &end);
	while (end && std::isspace(static_cast<unsigned char>(*end)))
		++end;

	if (end == begin || !end || *end != '\0' || errno == ERANGE || !std::isfinite(parsed))
		return false;

	result = parsed;
	return true;
}
} // namespace Detail

inline State Resolve(const Input& input)
{
	State state{};

	const bool fsrTemporalActive = input.fsr3Available && input.fsrEnabled && (input.renderScale < kTemporalRenderScaleThreshold || input.fsrNativeAA);
	// DLSS is ALWAYS temporal when enabled — its activation and scale come
	// from the DLSS preset/override, never from the FSR render scale.
	const bool dlssTemporalActive = input.dlssAvailable && !fsrTemporalActive && input.dlssEnabled;
	const bool dlaaPostActive = input.dlaaEnabled;

	state.active = fsrTemporalActive || dlssTemporalActive || dlaaPostActive;
	if (fsrTemporalActive) {
		state.method = input.fsrNativeAA ? OCU_EXTERNAL_UPSCALER_FSR_NATIVE_AA : OCU_EXTERNAL_UPSCALER_FSR3;
		state.renderScale = input.fsrNativeAA ? 1.0f : std::clamp(input.renderScale, 0.1f, 1.0f);
	} else if (dlssTemporalActive) {
		state.method = input.dlssPreset == 4 ? OCU_EXTERNAL_UPSCALER_DLAA : OCU_EXTERNAL_UPSCALER_DLSS;
		state.renderScale = (input.dlssPreset == 4) ? 1.0f : std::clamp(input.dlssRenderScale, 0.1f, 1.0f);
	} else if (dlaaPostActive) {
		state.method = OCU_EXTERNAL_UPSCALER_DLAA;
	}

	if (input.dlssAvailable && input.dlssEnabled)
		state.flags |= OCU_EXTERNAL_UPSCALER_FLAG_DLSS;
	if (input.fsr3Available && input.fsrEnabled)
		state.flags |= OCU_EXTERNAL_UPSCALER_FLAG_FSR3;
	if (input.fsr3Available && input.fsrEnabled && input.fsrNativeAA)
		state.flags |= OCU_EXTERNAL_UPSCALER_FLAG_FSR_NATIVE_AA;
	if (input.dlaaEnabled || (input.dlssAvailable && input.dlssEnabled && input.dlssPreset == 4)) {
		state.flags |= OCU_EXTERNAL_UPSCALER_FLAG_DLAA;
	}
	if (input.aswEnabled)
		state.flags |= OCU_EXTERNAL_UPSCALER_FLAG_ASW_ENABLED;

	if (input.dlssAvailable && input.dlssEnabled)
		state.dlssPreset = static_cast<uint32_t>(input.dlssPreset);

	if (!input.mipBiasEnabled)
		return state;

	const std::string mipBiasMode = Detail::NormalizeMipBiasMode(input.mipBiasMode);
	if (mipBiasMode == "off" || mipBiasMode == "false" || mipBiasMode == "disabled" || mipBiasMode == "none") {
		return state;
	}

	if (!mipBiasMode.empty() && mipBiasMode != "auto" && mipBiasMode != "default") {
		float fixedMipBias = 0.0f;
		if (Detail::TryParseFiniteFloat(mipBiasMode, fixedMipBias)) {
			state.mipBias = fixedMipBias;
			return state;
		}
	}

	if (!fsrTemporalActive && !dlssTemporalActive)
		return state;

	const float methodOffset = fsrTemporalActive ? input.fsr3MipBiasOffset : input.dlssMipBiasOffset;
	const float effectiveOffset = input.mipBiasOffset + methodOffset;
	const float mipBias = std::log2(std::clamp(state.renderScale, 0.1f, 1.0f)) + effectiveOffset;

	state.mipBiasOffset = std::isfinite(effectiveOffset) ? effectiveOffset : 0.0f;
	state.mipBias = std::isfinite(mipBias) ? mipBias : 0.0f;
	return state;
}
} // namespace ExternalUpscalerConfig
