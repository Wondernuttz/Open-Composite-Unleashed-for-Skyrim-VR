#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// Tapered laser beam texture (2026-07-25, VD-style lasers).
//
// Layout: the texture's V axis runs ALONG the beam — the beam quad's local +Y
// points at the far end, so row 0 (top) = tip, last row = controller end.
// The profile (user spec): constant ~3mm beam for most of its length, then
// the last stretch tapers into a fully transparent POINT at the tip. Soft
// edges + a subtle hot core throughout. Colours are PREMULTIPLIED — quad
// layers without the UNPREMULTIPLIED flag composite premultiplied, which is
// also why the old flat 4x4 fill looked slightly wrong at its edges.
namespace beamtex {

constexpr int kW = 32;
constexpr int kH = 256;

inline void Fill(std::vector<uint32_t>& px, uint8_t r, uint8_t g, uint8_t b, uint8_t baseAlpha)
{
	px.assign((size_t)kW * kH, 0);
	// Fraction of the beam (from the tip) that tapers/fades to the point
	constexpr float kTipZone = 0.30f;
	for (int y = 0; y < kH; y++) {
		// t: 0 at the tip (top) -> 1 at the controller (bottom)
		float t = (float)y / (kH - 1);
		// Constant width along the shaft; narrow to a point inside the tip zone
		float tip = (t < kTipZone) ? (t / kTipZone) : 1.0f; // 0 at tip -> 1 at zone end
		float half = (0.04f + 0.96f * tip) * (kW * 0.5f - 1.0f);
		// Solid along the shaft; alpha runs out to fully transparent at the tip
		float lenA = tip * tip; // eased fade, 0 exactly at the tip
		for (int x = 0; x < kW; x++) {
			float d = fabsf(x - (kW - 1) * 0.5f) / half; // 0 = core, 1 = edge
			if (d >= 1.0f)
				continue;
			float edge = 1.0f - d * d; // parabolic soft edge
			float a = (baseAlpha / 255.0f) * lenA * edge;
			// Hot core: blend toward white in the inner 35%
			float core = (d < 0.35f) ? (1.0f - d / 0.35f) : 0.0f;
			float cr = (r + (255 - r) * core * 0.8f) / 255.0f;
			float cg = (g + (255 - g) * core * 0.8f) / 255.0f;
			float cb = (b + (255 - b) * core * 0.8f) / 255.0f;
			// Premultiply
			uint8_t pr = (uint8_t)(cr * a * 255.0f + 0.5f);
			uint8_t pg = (uint8_t)(cg * a * 255.0f + 0.5f);
			uint8_t pb = (uint8_t)(cb * a * 255.0f + 0.5f);
			uint8_t pa = (uint8_t)(a * 255.0f + 0.5f);
			px[(size_t)y * kW + x] = pr | (pg << 8) | (pb << 16) | ((uint32_t)pa << 24);
		}
	}
}

} // namespace beamtex
