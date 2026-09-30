#pragma once

#include <string>

namespace ocu_foveation {
// Stable IDs shared with the density-mask shader; never use NVAPI enum values here.
enum class Rate : unsigned { X1x1, X1x2, X2x1, X2x2, X2x4, X4x2, X4x4 };
struct Footprint { unsigned x; unsigned y; };
inline Footprint Dimensions(Rate rate) {
    switch (rate) {
    case Rate::X1x2: return {1, 2};
    case Rate::X2x1: return {2, 1};
    case Rate::X2x2: return {2, 2};
    case Rate::X2x4: return {2, 4};
    case Rate::X4x2: return {4, 2};
    case Rate::X4x4: return {4, 4};
    default: return {1, 1};
    }
}
inline Rate ParseRate(const std::string& value) {
    if (value == "1x2") return Rate::X1x2;
    if (value == "2x1") return Rate::X2x1;
    if (value == "2x2") return Rate::X2x2;
    if (value == "2x4") return Rate::X2x4;
    if (value == "4x2") return Rate::X4x2;
    if (value == "4x4") return Rate::X4x4;
    return Rate::X1x1; // Malformed/unknown input must not increase coarseness.
}
inline const char* RateName(Rate rate) {
    switch (rate) {
    case Rate::X1x2: return "1x2";
    case Rate::X2x1: return "2x1";
    case Rate::X2x2: return "2x2";
    case Rate::X2x4: return "2x4";
    case Rate::X4x2: return "4x2";
    case Rate::X4x4: return "4x4";
    default: return "1x1";
    }
}
struct RingRates {
    Rate inner = Rate::X1x1;
    Rate mid = Rate::X2x1;
    Rate outer = Rate::X2x2;
    bool operator==(const RingRates& r) const { return inner == r.inner && mid == r.mid && outer == r.outer; }
    bool operator!=(const RingRates& r) const { return !(*this == r); }
};
inline Rate CapHalf(Rate rate, bool favorHorizontal) {
    const auto size = Dimensions(rate);
    if (size.x * size.y <= 2) return rate;
    if (size.x != size.y) return size.x > size.y ? Rate::X2x1 : Rate::X1x2;
    return favorHorizontal ? Rate::X2x1 : Rate::X1x2;
}
inline RingRates ResolveRates(bool eyeTracked, bool custom, bool compatibility, bool favorHorizontal,
    RingRates requested) {
    const Rate half = favorHorizontal ? Rate::X2x1 : Rate::X1x2;
    if (!eyeTracked || !custom)
        return {Rate::X1x1, half, compatibility ? half : Rate::X2x2};
    if (compatibility)
        return {CapHalf(requested.inner, favorHorizontal), CapHalf(requested.mid, favorHorizontal),
            CapHalf(requested.outer, favorHorizontal)};
    return requested;
}
}
