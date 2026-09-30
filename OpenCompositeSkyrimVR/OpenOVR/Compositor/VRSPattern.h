#pragma once

#include <cmath>
#include <cstdint>

namespace ocu_vrs_pattern {

enum class Level : uint8_t {
	Full = 0,    // 1x1
	Half = 1,    // 2x1 or 1x2
	Quarter = 2 // 2x2
};

inline int TileCount(int pixels, int tileSize)
{
	return pixels > 0 && tileSize > 0 ? (pixels + tileSize - 1) / tileSize : 0;
}

// Convert a full-render-target pixel into normalized coordinates inside one
// submitted eye region. This keeps stereo-atlas layout separate from gaze/FOV
// math and works for horizontal, vertical, or asymmetric eye bounds.
inline bool NormalizeInEyeRegion(float pixelX, float pixelY,
    int left, int top, int width, int height, float& eyeU, float& eyeV)
{
	if (!std::isfinite(pixelX) || !std::isfinite(pixelY) ||
	    width <= 0 || height <= 0 || pixelX < left || pixelY < top ||
	    pixelX >= left + width || pixelY >= top + height)
		return false;
	eyeU = (pixelX - left) / (float)width;
	eyeV = (pixelY - top) / (float)height;
	return true;
}

// Skyrim's terrain and alpha-tested foliage shaders can produce severe
// derivative/mip artifacts under 2x2 coarse shading. Compatibility mode keeps
// foveation active but caps every peripheral tile at half rate.
inline Level SelectLevel(float distance, float innerRadius, float midRadius,
    bool compatibilityMode)
{
	if (!std::isfinite(distance) || !std::isfinite(innerRadius) ||
	    !std::isfinite(midRadius))
		return Level::Full;

	const float inner = innerRadius < 0.0f ? 0.0f : innerRadius;
	const float mid = midRadius < inner ? inner : midRadius;

	if (distance < inner)
		return Level::Full;
	if (compatibilityMode || distance < mid)
		return Level::Half;
	return Level::Quarter;
}

} // namespace ocu_vrs_pattern
