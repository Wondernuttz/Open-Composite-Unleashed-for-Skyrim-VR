#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ocu_foveation {

// Internal scene/presentation policy. This is not part of the renderer API.
struct Blackout {
    bool middle = false;
    bool outer = false;
    bool cutoff = false;
    float cutoffRadius = 1.f;
    float guardPixels = 16.f;
    bool Active() const { return middle || outer || cutoff; }
};

inline double BlackoutCutoff(const Blackout& mask, double middle)
{
    const double radius = std::isfinite(mask.cutoffRadius) ?
        std::clamp(double(mask.cutoffRadius), .1, 1.5) : 1.;
    return (std::max)(radius, middle);
}

inline bool ValidBlackoutGeometry(double cx, double cy, double inner,
    double middle, double scale)
{
    return std::isfinite(cx) && std::isfinite(cy) && cx >= 0 && cx <= 1 &&
        cy >= 0 && cy <= 1 && std::isfinite(inner) && std::isfinite(middle) &&
        inner >= .1 && inner <= 1 && middle >= inner && middle <= 1.5 &&
        std::isfinite(scale) && scale >= .5 && scale <= 2;
}

inline bool BlackoutInterval(const Blackout& mask, double inner, double middle,
    double nearest, double farthest)
{
    const double cutoff = BlackoutCutoff(mask, middle);
    if (mask.middle && (mask.outer || (mask.cutoff && cutoff == middle)))
        return nearest > inner;
    return (mask.outer && nearest > middle) || (mask.cutoff && nearest > cutoff) ||
        (mask.middle && nearest > inner && farthest <= middle);
}

// Proves containment of a complete raster tile, including its sampling border.
// It does not establish that later global/temporal consumers ignore this area.
inline bool WholeBlackoutTile(const Blackout& mask, double cx, double cy,
    double inner, double middle, double scale, double left, double top,
    double tileWidth, double tileHeight, double eyeWidth, double eyeHeight)
{
    if (!mask.Active() || !ValidBlackoutGeometry(cx, cy, inner, middle, scale) ||
        !std::isfinite(mask.guardPixels) || mask.guardPixels < 0 ||
        !std::isfinite(left) || !std::isfinite(top) || !std::isfinite(tileWidth) ||
        !std::isfinite(tileHeight) || !std::isfinite(eyeWidth) || !std::isfinite(eyeHeight) ||
        tileWidth <= 0 || tileHeight <= 0 || eyeWidth <= 0 || eyeHeight <= 0 ||
        left < 0 || top < 0 || left + tileWidth > eyeWidth || top + tileHeight > eyeHeight)
        return false;
    const double x0 = 2 * ((left - mask.guardPixels) / eyeWidth - cx) / scale;
    const double x1 = 2 * ((left + tileWidth + mask.guardPixels) / eyeWidth - cx) / scale;
    const double y0 = 2 * ((top - mask.guardPixels) / eyeHeight - cy);
    const double y1 = 2 * ((top + tileHeight + mask.guardPixels) / eyeHeight - cy);
    const double nearX = std::clamp(0., x0, x1), nearY = std::clamp(0., y0, y1);
    const double farX = (std::max)(std::abs(x0), std::abs(x1));
    const double farY = (std::max)(std::abs(y0), std::abs(y1));
    return BlackoutInterval(mask, inner, middle,
        std::hypot(nearX, nearY), std::hypot(farX, farY));
}

struct BlackoutFrame {
    Blackout mask;
    float centers[2][2]{{.5f, .5f}, {.5f, .5f}};
    float inner = .2f, middle = .4f, horizontalScale = 1.f;
    float sceneEyeSize[2][2]{};
    std::uint64_t frameId = 0;
    bool Active() const {
        return mask.Active() && frameId &&
            ValidBlackoutGeometry(centers[0][0], centers[0][1], inner, middle, horizontalScale) &&
            ValidBlackoutGeometry(centers[1][0], centers[1][1], inner, middle, horizontalScale);
    }
};

// SolveSource bounds its iterative roots to 0.12 UV, while initial foreground
// probes may reach 64 source pixels per axis. Include bilinear support as well.
inline bool BlackoutSupportsDapaSource(const BlackoutFrame& frame, int eye,
    unsigned width, unsigned height)
{
    if (!frame.Active()) return true;
    if (eye < 0 || eye > 1 || !width || !height ||
        !std::isfinite(frame.mask.guardPixels) || frame.mask.guardPixels < 0) return false;
    const double dimensions[2]{double(width), double(height)};
    for (int axis = 0; axis < 2; ++axis) {
        const double sceneSize = frame.sceneEyeSize[eye][axis];
        if (!std::isfinite(sceneSize) || sceneSize <= 0) return false;
        const double needed = (std::max)(.12, 64. / dimensions[axis]) + .5 / dimensions[axis] + 1e-6;
        if (double(frame.mask.guardPixels) / sceneSize <= needed) return false;
    }
    return true;
}

} // namespace ocu_foveation
