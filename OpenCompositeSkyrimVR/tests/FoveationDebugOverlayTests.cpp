#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include "DrvOpenXR/FoveationDebugOverlay.h"
#include "OpenOVR/Misc/FoveationDebugRings.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace ocu_foveation_debug;
using namespace ocu_effect_foveation;
static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT value) { Check(SUCCEEDED(value), "D3D11 call failed"); }
static ID3D11Device* mockDevice;
static ComPtr<ID3D11Texture2D> mockImage;
static bool pendingAcquire = false, waited = false;
static XrResult nextWait = XR_SUCCESS;
static int64_t offeredFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
static unsigned acquires = 0, waits = 0, releases = 0, destroys = 0, formatQueries = 0;
static constexpr auto ChainValue = uintptr_t(1);

extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats)
{
    ++formatQueries; *count = 1; if (capacity) formats[0] = offeredFormat; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output)
{
    Check(info->width == Width && info->height == Height && info->arraySize == 1, "packed eye image dimensions");
    Check(info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, "upload usage declared");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info->width; desc.Height = info->height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = static_cast<DXGI_FORMAT>(info->format);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HR(mockDevice->CreateTexture2D(&desc, nullptr, mockImage.ReleaseAndGetAddressOf()));
    *output = reinterpret_cast<XrSwapchain>(ChainValue);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain)
{
    ++destroys; mockImage.Reset(); pendingAcquire = waited = false; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    *count = 1;
    if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture = mockImage.Get();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    Check(!pendingAcquire, "never reacquire a timed-out image");
    ++acquires; pendingAcquire = true; waited = false; *index = 0; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo* info)
{
    Check(pendingAcquire && !waited && info->timeout == 0, "correct nonblocking wait order");
    ++waits; waited = nextWait == XR_SUCCESS; return nextWait;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*)
{
    Check(pendingAcquire && waited, "never release before successful wait");
    ++releases; pendingAcquire = waited = false; return XR_SUCCESS;
}
}

static Snapshot Profile()
{
    Snapshot profile{};
    profile.structSize = sizeof(profile); profile.version = Version;
    profile.mode = Mode::EyeTracked; profile.shape = Shape::UVRadialHalfExtent;
    profile.centerUV[0][0] = 0.33f; profile.centerUV[0][1] = 0.5f;
    profile.centerUV[1][0] = 0.67f; profile.centerUV[1][1] = 0.44f;
    profile.innerRadius = 0.6f; profile.midRadius = 0.8f;
    return profile;
}
static std::vector<uint32_t> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<uint32_t> pixels(desc.Width * desc.Height);
    for (unsigned row = 0; row < desc.Height; ++row)
        std::memcpy(pixels.data() + row * desc.Width, static_cast<char*>(mapped.pData) + row * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0); return pixels;
}
static void CheckPixels(const Snapshot& profile, bool bgra)
{
    std::vector<uint32_t> pixels; Rasterize(profile, bgra, pixels);
    unsigned count = 0;
    for (int eye = 0; eye < 2; ++eye) {
        for (int y = 0; y < Height; ++y) for (int x = 0; x < EyeSize; ++x) {
            const auto pixel = pixels[y * Width + eye * EyeSize + x];
            if (!(pixel >> 24)) continue;
            ++count;
            const float radius = 2 * std::hypot((x + 0.5f) / EyeSize - profile.centerUV[eye][0],
                (y + 0.5f) / EyeSize - profile.centerUV[eye][1]);
            Check(std::min(std::abs(radius - profile.innerRadius), std::abs(radius - profile.midRadius)) < 0.012f,
                "every outline pixel follows its own eye's configured UV boundary");
            Check(((pixel >> (bgra ? 16 : 0)) & 255) == (pixel >> 24), "red channel uses premultiplied alpha");
        }
    }
    Check(count > 2000 && count < 25000, "thin outlines leave the eye image transparent");
}

static void Run(D3D_DRIVER_TYPE driver, const char* output)
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr, driver, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    mockDevice = device.Get();
    offeredFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    nextWait = XR_SUCCESS;
    FoveationDebugOverlay overlay;
    auto profile = Profile();
    XrCompositionLayerProjectionView sceneViews[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for (int eye = 0; eye < 2; ++eye) {
        sceneViews[eye].pose.orientation.w = 1;
        sceneViews[eye].pose.position.x = eye ? 0.03f : -0.03f;
        sceneViews[eye].fov = eye ? XrFovf{-0.6f, 1.0f, 0.85f, -0.7f} : XrFovf{-0.9f, 0.8f, 0.9f, -0.8f};
        sceneViews[eye].next = reinterpret_cast<const void*>(uintptr_t(42));
    }
    XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    scene.space = reinterpret_cast<XrSpace>(uintptr_t(2)); scene.viewCount = 2; scene.views = sceneViews;
    const auto session = reinterpret_cast<XrSession>(uintptr_t(3));
    const unsigned startingAcquires = acquires;
    Check(!overlay.PositionOverScene(scene) && !overlay.Layer(0) && acquires == startingAcquires, "disabled/unused overlay allocates no swapchain");

    D3D11_TEXTURE2D_DESC sentinelDesc{};
    sentinelDesc.Width = sentinelDesc.Height = 16; sentinelDesc.ArraySize = sentinelDesc.MipLevels = sentinelDesc.SampleDesc.Count = 1;
    sentinelDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sentinelDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> sentinel; HR(device->CreateTexture2D(&sentinelDesc, nullptr, &sentinel));
    ComPtr<ID3D11RenderTargetView> sentinelRTV; HR(device->CreateRenderTargetView(sentinel.Get(), nullptr, &sentinelRTV));
    const float sentinelColor[4]{0.1f, 0.2f, 0.3f, 1}; context->ClearRenderTargetView(sentinelRTV.Get(), sentinelColor);
    auto* rtv = sentinelRTV.Get(); context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT viewport{3, 4, 7, 8, 0.2f, 0.9f}; context->RSSetViewports(1, &viewport);
    const auto sentinelBefore = Read(device.Get(), context.Get(), sentinel.Get());
    Check(overlay.Update(session, device.Get(), profile), "valid tracked profile updates");
    ComPtr<ID3D11RenderTargetView> after; context->OMGetRenderTargets(1, &after, nullptr);
    Check(after.Get() == sentinelRTV.Get(), "game render target binding is unchanged");
    D3D11_VIEWPORT viewportAfter{}; UINT count = 1; context->RSGetViewports(&count, &viewportAfter);
    Check(count == 1 && std::memcmp(&viewport, &viewportAfter, sizeof(viewport)) == 0, "game viewport is unchanged");
    Check(Read(device.Get(), context.Get(), sentinel.Get()) == sentinelBefore, "game/upscaler/DAPA source pixels are unchanged");
    Check(overlay.PositionOverScene(scene), "eye overlays available after release");
    const auto* left = reinterpret_cast<const XrCompositionLayerQuad*>(overlay.Layer(0));
    const auto* right = reinterpret_cast<const XrCompositionLayerQuad*>(overlay.Layer(1));
    Check(left && right && !overlay.Layer(2), "exactly two debug layers");
    for (const auto* layer : {left, right}) {
        Check(layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD, "debug never submits a second scene projection");
        Check(layer->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT && !layer->next, "transparent quad with no depth attachment");
        Check(layer->space == scene.space, "quad uses submitted scene space");
    }
    Check(left->eyeVisibility == XR_EYE_VISIBILITY_LEFT && right->eyeVisibility == XR_EYE_VISIBILITY_RIGHT,
        "each eye sees only its own gaze rings");
    Check(left->subImage.imageRect.offset.x == 0 && right->subImage.imageRect.offset.x == EyeSize, "eye atlas does not cross eyes");
    const float centerX = (std::tan(sceneViews[1].fov.angleLeft) + std::tan(sceneViews[1].fov.angleRight)) * 0.5f;
    const float centerY = (std::tan(sceneViews[1].fov.angleUp) + std::tan(sceneViews[1].fov.angleDown)) * 0.5f;
    for (const auto* layer : {left, right}) {
        const auto& view = sceneViews[layer == left ? 0 : 1];
        for (float u : {0.0f, 0.2f, 0.5f, 0.8f, 1.0f}) {
            const float quadX = layer->pose.position.x - view.pose.position.x + (u - 0.5f) * layer->size.width;
            const float rayX = std::tan(view.fov.angleLeft) * (1-u) + std::tan(view.fov.angleRight) * u;
            const float quadY = layer->pose.position.y - view.pose.position.y + (0.5f-u) * layer->size.height;
            const float rayY = std::tan(view.fov.angleUp) * (1-u) + std::tan(view.fov.angleDown) * u;
            Check(std::abs(quadX-rayX) < 0.00001f && std::abs(quadY-rayY) < 0.00001f,
                "quad texels match the original asymmetric eye projection, including corners");
        }
        Check(layer->pose.position.z == -1, "quad lies in front of its eye");
    }
    const auto acquiredBeforeSynthetic = acquires;
    sceneViews[1].pose.position.x = 0.05f;
    Check(overlay.PositionOverScene(scene), "synthetic scene accepted");
    Check(std::abs(right->pose.position.x - (0.05f + centerX)) < 0.00001f && acquires == acquiredBeforeSynthetic,
        "DAPA reuses released image with its own eye pose");
    sceneViews[1].pose.orientation = {0, std::sqrt(0.5f), 0, std::sqrt(0.5f)};
    Check(overlay.PositionOverScene(scene) && std::abs(right->pose.position.x - (0.05f - 1.0f)) < 0.00001f &&
        std::abs(right->pose.position.z + centerX) < 0.00001f && std::abs(right->pose.position.y-centerY) < 0.00001f,
        "head/canted-eye rotation rotates the frustum offset as well as the quad");
    sceneViews[1].pose.orientation = {0,0,0,1};
    sceneViews[1].fov.angleUp = std::numeric_limits<float>::quiet_NaN();
    Check(!overlay.PositionOverScene(scene) && !overlay.Layer(0) && !overlay.Layer(1), "invalid eye view skips both overlays");
    sceneViews[1].fov.angleUp = 0.85f;
    Check(overlay.PositionOverScene(scene), "valid eye view recovers");
    auto actual = Read(device.Get(), context.Get(), mockImage.Get());
    std::vector<uint32_t> expected; Rasterize(profile, false, expected);
    Check(actual == expected, "actual GPU overlay pixels match both profile rings");
    if (output) { std::ofstream file(output, std::ios::binary); file.write(reinterpret_cast<const char*>(actual.data()), actual.size() * 4); }

    nextWait = XR_TIMEOUT_EXPIRED;
    const auto beforeReleases = releases;
    Check(!overlay.Update(session, device.Get(), profile) && !overlay.PositionOverScene(scene), "busy image skips overlay instead of showing stale gaze");
    Check(releases == beforeReleases && Read(device.Get(), context.Get(), mockImage.Get()) == actual, "timeout neither releases nor writes image");
    const auto beforeAcquires = acquires;
    nextWait = XR_SUCCESS; profile.mode = Mode::Fixed;
    Check(overlay.Update(session, device.Get(), profile) && acquires == beforeAcquires, "same acquired image recovers on next successful wait");
    auto fixed = Read(device.Get(), context.Get(), mockImage.Get());
    Check(std::any_of(fixed.begin(), fixed.end(), [](auto pixel) { return (pixel & 0xff00u) != 0; }), "fixed fallback uses amber");
    profile.mode = Mode::Disabled;
    Check(overlay.Update(session, device.Get(), profile), "disabled profile shows loss marker");
    actual = Read(device.Get(), context.Get(), mockImage.Get());
    for (int y = 0; y < Height; ++y) for (int x = 0; x < Width; ++x)
        if (actual[y * Width + x]) Check(std::abs(x % EyeSize - EyeSize / 2) < 9 && std::abs(y - EyeSize / 2) < 9, "loss clears old rings, retaining only center X");
    const auto beforeDestroy = destroys;
    overlay.Reset(); Check(destroys == beforeDestroy + 1 && !mockImage, "session reset destroys its swapchain");
    offeredFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto beforeUnsupported = formatQueries;
    Check(!overlay.Update(session, device.Get(), Profile()), "unsupported overlay format fails closed");
    Check(!overlay.Update(session, device.Get(), Profile()) && formatQueries == beforeUnsupported + 2, "unsupported runtime does not retry creation every frame");
    overlay.Reset(); offeredFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    Check(overlay.Update(session, device.Get(), Profile()), "BGRA-only runtime supported");
    Rasterize(Profile(), true, expected);
    Check(Read(device.Get(), context.Get(), mockImage.Get()) == expected, "BGRA red remains red on GPU");
    context->ClearState();
}

int main(int argc, char** argv)
{
    try {
        CheckPixels(Profile(), false); CheckPixels(Profile(), true);
        auto clipped = Profile(); clipped.centerUV[0][0] = 0; clipped.centerUV[1][0] = 1;
        CheckPixels(clipped, false);
        auto bad = Profile(); bad.centerUV[0][0] = std::numeric_limits<float>::quiet_NaN();
        Check(!Active(bad), "invalid coordinates rejected");
        Run(D3D_DRIVER_TYPE_WARP, argc > 1 ? argv[1] : nullptr);
        Run(D3D_DRIVER_TYPE_HARDWARE, nullptr);
        std::printf("PASS: %u checks; WARP and hardware GPU ring pixels, eye-only quads, asymmetric/rotated frustum alignment, alpha/BGRA, unchanged scene/state, DAPA reuse, timeout recovery, loss/fallback and session cleanup. OpenXR calls are mocked.\n", checks);
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
