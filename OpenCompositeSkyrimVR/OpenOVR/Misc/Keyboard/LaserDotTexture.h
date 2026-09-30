#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Soft laser endpoint modeled after PrismaUI's warm circular cursor. The
// texture is premultiplied because OCU's OpenXR quad layers intentionally do
// not request XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT. Keeping RGB at
// zero when alpha is zero also prevents runtimes from exposing the quad as a
// square around the cursor.
namespace laserdot {

constexpr int kSize = 64;

struct Color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

inline float RadialFalloff(float distance, float innerRadius, float outerRadius)
{
	if (distance <= innerRadius)
		return 1.0f;
	if (distance >= outerRadius)
		return 0.0f;
	const float t = (distance - innerRadius) / (outerRadius - innerRadius);
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

inline void Composite(float& outR, float& outG, float& outB, float& outA,
    const Color& color, float alpha)
{
	alpha = std::clamp(alpha, 0.0f, 1.0f);
	const float remaining = 1.0f - alpha;
	outR = color.r / 255.0f * alpha + outR * remaining;
	outG = color.g / 255.0f * alpha + outG * remaining;
	outB = color.b / 255.0f * alpha + outB * remaining;
	outA = alpha + outA * remaining;
}

inline void Fill(std::vector<uint32_t>& pixels, bool bgra,
    Color core = { 255, 250, 240 }, Color innerGlow = { 255, 250, 240 },
    Color outerGlow = { 255, 200, 150 }, float opacityScale = 1.0f)
{
	pixels.assign(static_cast<size_t>(kSize) * kSize, 0);
	const float center = (kSize - 1) * 0.5f;
	const float radius = center;

	for (int y = 0; y < kSize; ++y) {
		for (int x = 0; x < kSize; ++x) {
			const float dx = (x - center) / radius;
			const float dy = (y - center) / radius;
			const float distance = std::sqrt(dx * dx + dy * dy);

			float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
			// PrismaUI uses a bright 12px core, an 8px warm halo and a wider,
			// softer peach halo. These normalized rings preserve that character
			// at any physical quad size without requiring more composition layers.
			Composite(r, g, b, a, outerGlow,
			    0.30f * opacityScale * RadialFalloff(distance, 0.30f, 0.98f));
			Composite(r, g, b, a, innerGlow,
			    0.60f * opacityScale * RadialFalloff(distance, 0.20f, 0.66f));
			Composite(r, g, b, a, core,
			    0.95f * opacityScale * RadialFalloff(distance, 0.23f, 0.34f));

			uint8_t pr = static_cast<uint8_t>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f));
			uint8_t pg = static_cast<uint8_t>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f));
			uint8_t pb = static_cast<uint8_t>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f));
			const uint8_t pa = static_cast<uint8_t>(std::round(std::clamp(a, 0.0f, 1.0f) * 255.0f));
			if (bgra)
				std::swap(pr, pb);
			pixels[static_cast<size_t>(y) * kSize + x] =
			    static_cast<uint32_t>(pr) |
			    (static_cast<uint32_t>(pg) << 8) |
			    (static_cast<uint32_t>(pb) << 16) |
			    (static_cast<uint32_t>(pa) << 24);
		}
	}
}

} // namespace laserdot
