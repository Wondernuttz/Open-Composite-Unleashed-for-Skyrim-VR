#pragma once

#include "EffectFoveationAPI.h"
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
            if (alpha <= (target >> 24)) continue;
            target = (alpha << 24) | (amber ? ((alpha * 3 / 5) << 8) : 0) |
                (bgra ? (alpha << 16) : alpha);
        }
    }
}

inline void Rasterize(const ocu_effect_foveation::Snapshot& profile, bool bgra,
    std::vector<uint32_t>& pixels)
{
    pixels.assign(Width * Height, 0);
    const bool active = Active(profile);
    const bool amber = !active || profile.mode == ocu_effect_foveation::Mode::Fixed;
    constexpr float tau = 6.28318530718f;
    for (int eye = 0; eye < 2; ++eye) {
        if (!active) {
            // No last-known gaze ring is drawn after loss or a disabled profile.
            for (int offset = -6; offset <= 6; ++offset) {
                Stamp(pixels, eye, EyeSize * 0.5f + offset, EyeSize * 0.5f + offset, true, bgra);
                Stamp(pixels, eye, EyeSize * 0.5f + offset, EyeSize * 0.5f - offset, true, bgra);
            }
            continue;
        }
        const float cx = profile.centerUV[eye][0] * EyeSize;
        const float cy = profile.centerUV[eye][1] * EyeSize;
        for (int ring = 0; ring < 2; ++ring) {
            const float radius = (ring ? profile.midRadius : profile.innerRadius) * EyeSize * 0.5f;
            const int samples = static_cast<int>(std::ceil(tau * radius * 1.5f));
            for (int sample = 0; sample < samples; ++sample) {
                // The configured middle boundary remains visible even when a rate cap
                // makes middle and outer shading rates equal.
                if (ring && (static_cast<int>(sample * tau * radius / samples) / 9) % 2) continue;
                const float angle = tau * sample / samples;
                Stamp(pixels, eye, cx + radius * std::cos(angle), cy + radius * std::sin(angle), amber, bgra);
            }
        }
    }
}
} // namespace ocu_foveation_debug
