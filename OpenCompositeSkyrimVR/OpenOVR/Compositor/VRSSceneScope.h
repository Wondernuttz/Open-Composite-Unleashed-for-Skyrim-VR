#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace ocu_vrs_scope {

// KEEP means the supplied RTV/DSV arguments are ignored, not an empty binding.
template<class Visitor>
void WithRenderTargets(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth, Visitor&& visitor)
{
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
        visitor(std::min(count, UINT(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)), views, depth);
        return;
    }
    ID3D11RenderTargetView* bound[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> boundDepth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, &boundDepth);
    visitor(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, bound, boundDepth.Get());
    for (auto* view : bound)
        if (view) view->Release();
}

template<class Region>
Region ScaleRegion(const Region& region, int sourceWidth, int sourceHeight, int width, int height)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || width <= 0 || height <= 0)
        return {};
    auto scale = [](int value, int from, int to) {
        return int(std::int64_t(value) * to / from);
    };
    const int left = scale(region.left, sourceWidth, width);
    const int top = scale(region.top, sourceHeight, height);
    return { left, top,
        scale(region.left + region.width, sourceWidth, width) - left,
        scale(region.top + region.height, sourceHeight, height) - top };
}

struct ViewportEyeRegion {
    float left = 0, top = 0, width = 0, height = 0;
};

// A viewport does not identify an eye or a camera. Accept only layouts whose
// bounds establish both eye ownership and the stereo-to-viewport transform.
// In particular, a small single viewport confined to one eye could represent
// either that eye or a reduced whole-stereo pass; do not guess which one.
template<class Region>
bool MapStereoViewports(int width, int height, const Region* eyes, UINT count,
    const D3D11_VIEWPORT* viewports, ViewportEyeRegion* result)
{
    if (width <= 0 || height <= 0 || !eyes || !viewports || !result || !count || count > 2)
        return false;
    auto same = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    auto region = [](const D3D11_VIEWPORT& viewport) {
        return ViewportEyeRegion{viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height};
    };
    auto inside = [](const ViewportEyeRegion& a, const auto& b) {
        return a.left >= b.left && a.top >= b.top &&
            a.left + a.width <= b.left + b.width && a.top + a.height <= b.top + b.height;
    };
    auto equal = [&](const ViewportEyeRegion& a, const auto& b) {
        return same(a.left, float(b.left)) && same(a.top, float(b.top)) &&
            same(a.width, float(b.width)) && same(a.height, float(b.height));
    };
    auto overlaps = [](const auto& a, const auto& b) {
        return a.left < b.left + b.width && a.left + a.width > b.left &&
            a.top < b.top + b.height && a.top + a.height > b.top;
    };
    const ViewportEyeRegion allocation{0, 0, float(width), float(height)};
    ViewportEyeRegion active[2]{};
    for (UINT i = 0; i < count; ++i) {
        active[i] = region(viewports[i]);
        if (!std::isfinite(active[i].left) || !std::isfinite(active[i].top) ||
            !std::isfinite(active[i].width) || !std::isfinite(active[i].height) ||
            active[i].width <= 0 || active[i].height <= 0 || !inside(active[i], allocation))
            return false;
    }
    auto baseline = [&]() {
        for (int e = 0; e < 2; ++e)
            result[e] = {float(eyes[e].left), float(eyes[e].top), float(eyes[e].width), float(eyes[e].height)};
        return true;
    };
    if (count == 2) {
        // Repeated full-atlas viewports do not change the submitted mapping.
        if (equal(active[0], allocation) && equal(active[1], allocation)) return baseline();
        if (overlaps(active[0], active[1])) return false;
        const int firstEye = inside(active[0], eyes[0]) ? 0 : (inside(active[0], eyes[1]) ? 1 : -1);
        const int secondEye = inside(active[1], eyes[0]) ? 0 : (inside(active[1], eyes[1]) ? 1 : -1);
        if (firstEye < 0 || secondEye < 0 || firstEye == secondEye) return false;
        // Bounds determine ownership, including reversed viewport-array order.
        result[firstEye] = active[0]; result[secondEye] = active[1];
        return true;
    }
    if (equal(active[0], allocation) || equal(active[0], eyes[0]) || equal(active[0], eyes[1]))
        return baseline();
    // A shared viewport must span both known eyes and preserve the stereo
    // aspect. A <=50% SBS viewport at the origin remains ambiguous/full rate.
    if (!overlaps(active[0], eyes[0]) || !overlaps(active[0], eyes[1]) ||
        std::fabs(active[0].width / float(width) - active[0].height / float(height)) >
            1.0f / float(std::min(width, height)))
        return false;
    for (int e = 0; e < 2; ++e) {
        result[e] = {active[0].left + float(eyes[e].left) * active[0].width / width,
            active[0].top + float(eyes[e].top) * active[0].height / height,
            float(eyes[e].width) * active[0].width / width,
            float(eyes[e].height) * active[0].height / height};
    }
    return true;
}

class SceneScope {
public:
    void Reset()
    {
        depth_.Reset();
        submitted_.Reset();
        context_ = nullptr;
        width_ = height_ = 0;
    }

    void Arm(ID3D11DeviceContext* context, ID3D11Texture2D* depth,
        ID3D11Texture2D* submitted, UINT width, UINT height)
    {
        context_ = context;
        depth_ = depth;
        submitted_ = submitted;
        width_ = width;
        height_ = height;
    }

    bool UsesDepth() const { return depth_ != nullptr; }

    bool Matches(ID3D11DeviceContext* context, UINT count,
        ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) const
    {
        if (!context_ || context != context_ || !views || !width_ || !height_ ||
            count > D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)
            return false;
        if (depth_) {
            if (!depth) return false;
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            depth->GetResource(&resource);
            if (resource.Get() != depth_.Get()) return false;
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            depth->GetDesc(&desc);
            if (desc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || desc.Texture2D.MipSlice != 0)
                return false;
        }
        bool any = false;
        bool submittedBound = false;
        for (UINT i = 0; i < count; ++i) {
            if (!views[i]) continue;
            D3D11_RENDER_TARGET_VIEW_DESC viewDesc{};
            views[i]->GetDesc(&viewDesc);
            if (viewDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || viewDesc.Texture2D.MipSlice != 0)
                return false;
            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            views[i]->GetResource(&resource);
            if (FAILED(resource.As(&texture))) return false;
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width != width_ || desc.Height != height_ || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
                return false;
            any = true;
            submittedBound = submittedBound || texture.Get() == submitted_.Get();
        }
        // Main depth identifies geometry even when CSX draws to G-buffers or an
        // internal-resolution target rather than the eventual submitted color.
        return any && (depth_ || submittedBound);
    }

private:
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> submitted_;
    UINT width_ = 0, height_ = 0;
};
} // namespace ocu_vrs_scope
