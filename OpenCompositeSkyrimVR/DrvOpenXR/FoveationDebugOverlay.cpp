#if defined(SUPPORT_DX11)
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include "FoveationDebugOverlay.h"
#include "../OpenOVR/Misc/FoveationDebugRings.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

struct FoveationDebugOverlay::Impl {
    XrSession session = XR_NULL_HANDLE;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::vector<uint32_t> pixels;
    bool acquired = false, visible = false, failed = false, bgra = false;
    uint32_t index = 0;
    const char* status = "off";
    bool positioned = false;
    XrCompositionLayerQuad layers[LayerCount]{};

    void Fail(const char* reason) { failed = true; visible = false; status = reason; }
    ~Impl() { if (chain != XR_NULL_HANDLE) xrDestroySwapchain(chain); }

    bool Initialize(XrSession newSession, ID3D11Device* device)
    {
        session = newSession;
        if (!device || session == XR_NULL_HANDLE) { Fail("D3D11/session unavailable"); return false; }
        uint32_t count = 0;
        if (xrEnumerateSwapchainFormats(session, 0, &count, nullptr) != XR_SUCCESS || !count || count > 4096) {
            Fail("swapchain formats unavailable"); return false;
        }
        std::vector<int64_t> formats(count);
        if (xrEnumerateSwapchainFormats(session, count, &count, formats.data()) != XR_SUCCESS) {
            Fail("swapchain format query failed"); return false;
        }
        int64_t selected = 0;
        for (auto candidate : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
            if (std::find(formats.begin(), formats.end(), int64_t(candidate)) != formats.end()) {
                selected = candidate; break;
            }
        }
        if (!selected) { Fail("no supported RGBA/BGRA overlay format"); return false; }
        bgra = selected == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || selected == DXGI_FORMAT_B8G8R8A8_UNORM;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        create.format = selected;
        create.sampleCount = create.faceCount = create.arraySize = create.mipCount = 1;
        create.width = ocu_foveation_debug::Width;
        create.height = ocu_foveation_debug::Height;
        if (xrCreateSwapchain(session, &create, &chain) != XR_SUCCESS) {
            chain = XR_NULL_HANDLE; Fail("overlay swapchain creation failed"); return false;
        }
        count = 0;
        if (xrEnumerateSwapchainImages(chain, 0, &count, nullptr) != XR_SUCCESS || !count || count > 64) {
            Fail("overlay images unavailable"); return false;
        }
        images.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (xrEnumerateSwapchainImages(chain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) != XR_SUCCESS) {
            Fail("overlay image enumeration failed"); return false;
        }
        device->GetImmediateContext(context.GetAddressOf());
        if (!context) { Fail("D3D11 context unavailable"); return false; }
        return true;
    }
};

FoveationDebugOverlay::FoveationDebugOverlay() : impl(std::make_unique<Impl>()) {}
FoveationDebugOverlay::~FoveationDebugOverlay() = default;
void FoveationDebugOverlay::Reset() { impl = std::make_unique<Impl>(); }
const char* FoveationDebugOverlay::Status() const { return impl->status; }

bool FoveationDebugOverlay::Update(XrSession session, ID3D11Device* device,
    const ocu_effect_foveation::Snapshot& profile, float horizontalScale,
    bool drawRings, bool peripheralMask, float maskRadius, bool middleBlackout, bool outerBlackout)
{
    if (impl->session != XR_NULL_HANDLE && impl->session != session) Reset();
    auto& state = *impl;
    state.visible = false;
    state.positioned = false;
    const bool active = ocu_foveation_debug::Active(profile);
    const bool tracked = active && profile.mode == ocu_effect_foveation::Mode::EyeTracked;
    const bool masked = tracked && (peripheralMask || middleBlackout || outerBlackout);
    if (!drawRings && !masked) {
        // Hide a stale mask immediately on tracking loss. No transparent upload
        // or extra composition layers are needed when there is nothing to draw.
        state.status = "off: no active overlay";
        return false;
    }
    if (state.failed || (!state.chain && !state.Initialize(session, device))) return false;
    if (!state.acquired) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (xrAcquireSwapchainImage(state.chain, &acquire, &state.index) != XR_SUCCESS) {
            state.Fail("overlay acquire failed"); return false;
        }
        state.acquired = true;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 0;
    const XrResult waited = xrWaitSwapchainImage(state.chain, &wait);
    if (waited == XR_TIMEOUT_EXPIRED) { state.status = "overlay image busy (skipped)"; return false; }
    if (waited != XR_SUCCESS || state.index >= state.images.size() || !state.images[state.index].texture) {
        state.Fail("overlay wait/image failed"); return false;
    }
    ocu_foveation_debug::Rasterize(profile, state.bgra, state.pixels,
        horizontalScale, drawRings, peripheralMask, maskRadius, middleBlackout, outerBlackout);
    // No game texture, shader binding or context pipeline state is modified.
    state.context->UpdateSubresource(state.images[state.index].texture, 0, nullptr,
        state.pixels.data(), ocu_foveation_debug::Width * sizeof(uint32_t), 0);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (xrReleaseSwapchainImage(state.chain, &release) != XR_SUCCESS) {
        state.Fail("overlay release failed"); return false;
    }
    state.acquired = false;
    state.visible = true;
    state.status = !drawRings ? "tracked visibility mask" :
        !active ? "amber X: no active profile" : tracked ?
        (masked ? "red: tracked rings with visibility mask" : "red: tracked eye profile") : "amber: fixed fallback profile";
    return true;
}

bool FoveationDebugOverlay::PositionOverScene(const XrCompositionLayerProjection& scene)
{
    impl->positioned = false;
    if (!impl->visible || scene.viewCount != LayerCount || !scene.views || scene.space == XR_NULL_HANDLE) return false;
    XrCompositionLayerQuad placed[LayerCount]{};
    for (uint32_t eye = 0; eye < LayerCount; ++eye) {
        const auto& view = scene.views[eye];
        const auto& p = view.pose.position;
        auto q = view.pose.orientation;
        const float normSq = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (!std::isfinite(normSq) || std::abs(normSq - 1.0f) > 0.01f ||
            !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
        for (float angle : {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown})
            if (!std::isfinite(angle) || std::abs(angle) >= 1.5707f) return false;
        const float left = std::tan(view.fov.angleLeft), right = std::tan(view.fov.angleRight);
        const float up = std::tan(view.fov.angleUp), down = std::tan(view.fov.angleDown);
        if (right <= left || up <= down) return false;

        // One eye-only quad spans that eye's asymmetric frustum at one metre.
        // Keep Skyrim as the only projection layer: some runtime/API-layer
        // combinations replace it when a second projection is submitted.
        auto& quad = placed[eye];
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.space = scene.space;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.eyeVisibility = eye ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT;
        quad.size = {right - left, up - down};
        const float invNorm = 1.0f / std::sqrt(normSq);
        q.x *= invNorm; q.y *= invNorm; q.z *= invNorm; q.w *= invNorm;
        quad.pose.orientation = q;
        const XrVector3f local{(left + right) * 0.5f, (up + down) * 0.5f, -1.0f};
        const XrVector3f t{2*(q.y*local.z - q.z*local.y),
            2*(q.z*local.x - q.x*local.z), 2*(q.x*local.y - q.y*local.x)};
        quad.pose.position = {p.x + local.x + q.w*t.x + q.y*t.z - q.z*t.y,
            p.y + local.y + q.w*t.y + q.z*t.x - q.x*t.z,
            p.z + local.z + q.w*t.z + q.x*t.y - q.y*t.x};
        quad.subImage.swapchain = impl->chain;
        quad.subImage.imageArrayIndex = 0;
        quad.subImage.imageRect = {{static_cast<int>(eye) * ocu_foveation_debug::EyeSize, 0},
            {ocu_foveation_debug::EyeSize, ocu_foveation_debug::EyeSize}};
    }
    std::copy(std::begin(placed), std::end(placed), std::begin(impl->layers));
    impl->positioned = true;
    return true;
}

const XrCompositionLayerBaseHeader* FoveationDebugOverlay::Layer(uint32_t eye) const
{
    if (!impl->visible || !impl->positioned || eye >= LayerCount) return nullptr;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl->layers[eye]);
}
#endif
