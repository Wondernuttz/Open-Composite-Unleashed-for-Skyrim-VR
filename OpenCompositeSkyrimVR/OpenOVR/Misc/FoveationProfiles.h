#pragma once

#include <algorithm>
#include <cmath>

namespace ocu_foveation {
struct Radii { float inner; float mid; };

// Starting profiles, not universal perceptual thresholds. Values
// are normalized eye-texture radii, not degrees. Preserve explicit legacy
// sizes until a user saves independent profiles in the configurator.
inline Radii Resolve(bool eyeTracked, float legacyInner, float legacyMid,
    float fixedInner, float fixedMid, float eyeInner, float eyeMid)
{
    const Radii defaults = eyeTracked ? Radii{0.20f, 0.40f} : Radii{0.70f, 0.85f};
    auto choose = [](float explicitValue, float legacyValue, float fallback, float maximum) {
        const float value = std::isfinite(explicitValue) && explicitValue >= 0.0f ? explicitValue
            : (std::isfinite(legacyValue) && legacyValue >= 0.0f ? legacyValue : fallback);
        return std::clamp(value, 0.10f, maximum);
    };
    const float inner = choose(eyeTracked ? eyeInner : fixedInner, legacyInner, defaults.inner, 1.0f);
    const float mid = choose(eyeTracked ? eyeMid : fixedMid, legacyMid, defaults.mid, 1.5f);
    return {inner, (std::max)(inner, mid)};
}
}
