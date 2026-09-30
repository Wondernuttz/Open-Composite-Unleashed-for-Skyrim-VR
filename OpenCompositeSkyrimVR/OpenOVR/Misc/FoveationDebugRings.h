#pragma once

#include "EffectFoveationAPI.h"
#include "FoveationGeometrySettings.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ocu_foveation_debug {
constexpr int EyeSize = 512;
constexpr int Width = EyeSize * 2;
constexpr int Height = EyeSize;

inline bool Active(const ocu_effect_foveation::Snapshot& profile)
{
    using namespace ocu_effect_foveation;
    if (profile.structSize != sizeof(profile) || profile.version != Version ||
        profile.shape != Shape::UVRadialHalfExtent ||
        (profile.mode != Mode::EyeTracked && profile.mode != Mode::Fixed) ||
        !std::isfinite(profile.innerRadius) || !std::isfinite(profile.midRadius) ||
        profile.innerRadius < 0.1f || profile.innerRadius > 1.0f ||
        profile.midRadius < profile.innerRadius || profile.midRadius > 1.5f)
        return false;
    for (const auto& center : profile.centerUV)
        for (float value : center)
            if (!std::isfinite(value) || value < 0 || value > 1) return false;
    return true;
}

// Premultiplied alpha matches OpenXR's default source-alpha composition.
inline void Stamp(std::vector<uint32_t>& pixels, int eye, float x, float y, bool amber, bool bgra)
{
    constexpr float halfWidth = 1.25f;
    const int left = (std::max)(0, static_cast<int>(std::floor(x - halfWidth - 1)));
    const int right = (std::min)(EyeSize - 1, static_cast<int>(std::ceil(x + halfWidth + 1)));
    const int top = (std::max)(0, static_cast<int>(std::floor(y - halfWidth - 1)));
    const int bottom = (std::min)(EyeSize - 1, static_cast<int>(std::ceil(y + halfWidth + 1)));
    for (int py = top; py <= bottom; ++py) {
        for (int px = left; px <= right; ++px) {
            const float distance = std::hypot(px + 0.5f - x, py + 0.5f - y);
            const auto alpha = static_cast<uint32_t>(255.0f * std::clamp(halfWidth + 0.5f - distance, 0.0f, 1.0f));
            auto& target = pixels[py * Width + eye * EyeSize + px];
            // Red stores the previous line coverage, even over an opaque black
            // mask. Keep that mask opaque while antialiasing the diagnostic line.
            const auto previousCoverage = (target >> (bgra ? 16 : 0)) & 255u;
            if (alpha <= previousCoverage) continue;
            target = ((std::max)(target >> 24, alpha) << 24) | (amber ? ((alpha * 3 / 5) << 8) : 0) |
                (bgra ? (alpha << 16) : alpha);
        }
    }
}

struct PixelSpan { int begin = 0, end = 0; };

// Pixel-center interval inside an ellipse, including its boundary. A clipped
// empty interval also represents scanlines above or below the ellipse.
inline PixelSpan EllipseSpan(float cx, float dy, float width, float radius)
{
    const float horizontalSquared = radius * radius - dy * dy;
    if (horizontalSquared < 0.0f) return {};
    const float extent = width * std::sqrt(horizontalSquared);
    return {std::clamp(static_cast<int>(std::ceil(cx - extent - 0.5f)), 0, EyeSize),
        std::clamp(static_cast<int>(std::floor(cx + extent - 0.5f)) + 1, 0, EyeSize)};
}

inline void Rasterize(const ocu_effect_foveation::Snapshot& profile, bool bgra,
    std::vector<uint32_t>& pixels, float horizontalScale = 1.0f,
    bool drawRings = true, bool peripheralMask = false, float maskRadius = 1.0f,
    bool middleBlackout = false, bool outerBlackout = false)
{
    pixels.assign(Width * Height, 0);
    const bool active = Active(profile);
    const bool amber = !active || profile.mode == ocu_effect_foveation::Mode::Fixed;
    const bool tracked = active && profile.mode == ocu_effect_foveation::Mode::EyeTracked;
    const float width = tracked ? ocu_foveation::HorizontalScale(horizontalScale) : 1.0f;
    const bool mask = tracked && peripheralMask;
    const bool middleMask = tracked && middleBlackout;
    const bool outerMask = tracked && outerBlackout;
    const float visibleRadius = ocu_foveation::PeripheralMaskRadius(maskRadius, profile.midRadius) * EyeSize * 0.5f;
    constexpr float tau = 6.28318530718f;
    for (int eye = 0; eye < 2; ++eye) {
        if (!active) {
            // No last-known gaze ring is drawn after loss or a disabled profile.
            for (int offset = -6; drawRings && offset <= 6; ++offset) {
                Stamp(pixels, eye, EyeSize * 0.5f + offset, EyeSize * 0.5f + offset, true, bgra);
                Stamp(pixels, eye, EyeSize * 0.5f + offset, EyeSize * 0.5f - offset, true, bgra);
            }
            continue;
        }
        const float cx = profile.centerUV[eye][0] * EyeSize;
        const float cy = profile.centerUV[eye][1] * EyeSize;
        if (mask || middleMask || outerMask) {
            // These are visibility overlays, not rendering culls. Game
            // color/depth and temporal histories stay intact. Compute at most
            // three ellipse intersections per row, never per-pixel square roots.
            for (int y = 0; y < EyeSize; ++y) {
                auto row = pixels.begin() + y * Width + eye * EyeSize;
                const float dy = y + 0.5f - cy;
                const auto black = [&](int begin, int end) { std::fill(row + begin, row + end, 0xff000000u); };
                const auto outside = [&](PixelSpan span) { black(0, span.begin); black(span.end, EyeSize); };
                if (middleMask && outerMask) {
                    outside(EllipseSpan(cx, dy, width, profile.innerRadius * EyeSize * 0.5f));
                    continue; // Both selected regions also include the cutoff.
                }
                if (middleMask || outerMask) {
                    const auto middle = EllipseSpan(cx, dy, width, profile.midRadius * EyeSize * 0.5f);
                    if (outerMask) outside(middle);
                    else {
                        const auto inner = EllipseSpan(cx, dy, width, profile.innerRadius * EyeSize * 0.5f);
                        if (inner.begin == inner.end) black(middle.begin, middle.end);
                        else {
                            black(middle.begin, (std::min)(middle.end, inner.begin));
                            black((std::max)(middle.begin, inner.end), middle.end);
                        }
                    }
                }
                if (mask && !outerMask) outside(EllipseSpan(cx, dy, width, visibleRadius));
            }
        }
        for (int ring = 0; drawRings && ring < (mask ? 3 : 2); ++ring) {
            const float radius = ring == 2 ? visibleRadius :
                (ring ? profile.midRadius : profile.innerRadius) * EyeSize * 0.5f;
            const int samples = static_cast<int>(std::ceil(tau * radius * (std::max)(1.0f, width) * 1.5f));
            for (int sample = 0; sample < samples; ++sample) {
                // The configured middle boundary remains visible even when a rate cap
                // makes middle and outer shading rates equal.
                const int arcPixel = static_cast<int>(sample * tau * radius / samples);
                if (ring == 1 && (arcPixel / 9) % 2) continue;
                // A short dotted cutoff is distinct from the dashed middle.
                if (ring == 2 && arcPixel % 12 >= 3) continue;
                const float angle = tau * sample / samples;
                Stamp(pixels, eye, cx + radius * width * std::cos(angle), cy + radius * std::sin(angle), amber, bgra);
            }
        }
    }
}
} // namespace ocu_foveation_debug
