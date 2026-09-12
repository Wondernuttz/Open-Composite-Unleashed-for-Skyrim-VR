#pragma once
#include <cmath>
#include <cstdint>

namespace OcuIndexTrackpad {
// Bits: left upper/lower, right upper/lower. Defaults remain Menu/A.
// Customized halves use separate OpenVR DPad Right/Down ids (5/6), per hand.
inline uint64_t PressMask(int hand, float y, bool active, bool pressed,
    bool disabled, bool vrik, unsigned customRegions)
{
	if (hand < 0 || hand > 1 || !active || !pressed || disabled || vrik || !std::isfinite(y)) return 0;
	const bool upper = y > 0;
	const unsigned region = 1u << (hand * 2 + (upper ? 0 : 1));
	const unsigned button = (customRegions & region) ? (upper ? 5u : 6u) : (upper ? 1u : 7u);
	return uint64_t{1} << button;
}
}
