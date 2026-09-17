#pragma once

#include <algorithm>
#include <cmath>

namespace ocu_foveation {
inline float HorizontalScale(float value)
{ return std::isfinite(value) ? std::clamp(value, .5f, 2.f) : 1.f; }
inline float CenterOffset(float value)
{ return std::isfinite(value) ? std::clamp(value, -.25f, .25f) : 0.f; }
inline float PeripheralMaskRadius(float value, float middle)
{
    const float radius = std::isfinite(value) ? std::clamp(value, .1f, 1.5f) : 1.f;
    return (std::max)(radius, std::isfinite(middle) ? std::clamp(middle, .1f, 1.5f) : .1f);
}
inline void OffsetCenter(float& x, float& y, int eye, float horizontal, float vertical)
{
    // Positive horizontal adjustment moves each eye's center toward its temple.
    x = std::clamp(x + (eye == 0 ? -1.f : 1.f) * CenterOffset(horizontal), 0.f, 1.f);
    y = std::clamp(y + CenterOffset(vertical), 0.f, 1.f);
}
}
