#pragma once

#include <array>
#include <cstdint>

namespace oovr_laser_style {
// Straight sRGB palette shared by atlas generation and the pointer API.
inline constexpr std::array<std::array<std::uint8_t, 4>, 2> Beam = {{
    { 255, 240, 220, 200 }, { 55, 145, 255, 220 }
}};
inline constexpr std::array<std::array<std::uint8_t, 3>, 2> DotCore = {{
    { 255, 250, 240 }, { 225, 245, 255 }
}};
inline constexpr std::array<std::array<std::uint8_t, 3>, 2> DotInner = {{
    { 255, 250, 240 }, { 90, 175, 255 }
}};
inline constexpr std::array<std::array<std::uint8_t, 3>, 2> DotOuter = {{
    { 255, 200, 150 }, { 35, 105, 255 }
}};
inline constexpr float DotCoreAlpha = 0.95f;
}
