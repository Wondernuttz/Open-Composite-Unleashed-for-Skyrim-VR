#if defined(SUPPORT_DX11)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include "CableTrackingOverlay.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#pragma comment(lib, "gdi32.lib")

struct CableTrackingOverlay::Impl {
    static constexpr int Width = 640, Height = 192;
    ocu_cable::Counter counter;
    XrSession session = XR_NULL_HANDLE;
    XrSpace physical = XR_NULL_HANDLE;
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<XrTime> changes;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool failed = false, acquired = false, uploaded = false, bgra = false;
    bool showHeld = false, resetHeld = false, warned = false;
    uint32_t index = 0;
    ULONGLONG visibleUntil = 0, nextUpload = 0;
    HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ oldBitmap = nullptr;
    HFONT titleFont = nullptr, bodyFont = nullptr;
    uint32_t* pixels = nullptr;
    std::vector<uint32_t> rgba;

    ~Impl() {
        if (chain) xrDestroySwapchain(chain);
        if (physical) xrDestroySpace(physical);
        if (dc && oldBitmap) SelectObject(dc, oldBitmap);
        if (bitmap) DeleteObject(bitmap);
        if (titleFont) DeleteObject(titleFont);
        if (bodyFont) DeleteObject(bodyFont);
        if (dc) DeleteDC(dc);
    }
    bool Initialize(ID3D11Device* device) {
        uint32_t n = 0;
        if (!device || xrEnumerateSwapchainFormats(session, 0, &n, nullptr) != XR_SUCCESS || !n || n > 4096) return false;
        std::vector<int64_t> formats(n);
        if (xrEnumerateSwapchainFormats(session, n, &n, formats.data()) != XR_SUCCESS) return false;
        int64_t format = 0;
        for (auto f : {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                      DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM})
            if (std::find(formats.begin(), formats.end(), int64_t(f)) != formats.end()) { format = f; break; }
        if (!format) return false;
        bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM;
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        create.format = format; create.width = Width; create.height = Height;
        create.sampleCount = create.faceCount = create.arraySize = create.mipCount = 1;
        if (xrCreateSwapchain(session, &create, &chain) != XR_SUCCESS) return false;
        if (xrEnumerateSwapchainImages(chain, 0, &n, nullptr) != XR_SUCCESS || !n || n > 64) return false;
        images.resize(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (xrEnumerateSwapchainImages(chain, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())) != XR_SUCCESS) return false;
        device->GetImmediateContext(context.GetAddressOf());
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = Width; info.bmiHeader.biHeight = -Height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        if (!dc || !bitmap || !pixels || !context) return false;
        oldBitmap = SelectObject(dc, bitmap);
        titleFont = CreateFontW(-42,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        bodyFont = CreateFontW(-23,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        if (!titleFont || !bodyFont) return false;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.pose.orientation.w = 1;
        quad.pose.position = {0,-.24f,-.85f};
        quad.size = {.55f,.165f};
        quad.subImage.swapchain = chain;
        quad.subImage.imageRect = {{0,0},{Width,Height}};
        return true;
    }
    void Rasterize() {
        std::fill(pixels, pixels+Width*Height, 0xff10171a);
        SetBkMode(dc, TRANSPARENT);
        auto line = [&](const char* text, int y, HFONT font, COLORREF color) {
            auto old = SelectObject(dc, font); SetTextColor(dc,color);
            RECT rect{16,y,Width-16,y+52};
            DrawTextA(dc,text,-1,&rect,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
            SelectObject(dc,old);
        };
        const bool uncertain = counter.Uncertain();
        const auto green = RGB(92,245,143), amber = RGB(255,193,84);
        line("OCU  /  CABLE TRACKING", 4, bodyFont, green);
        char label[128];
        const double turns = counter.Turns();
        if (std::abs(turns) < .03) snprintf(label,sizeof(label),"Centered  -  0.00 turns");
        else snprintf(label,sizeof(label),"Unwind %s  %.2f turns",turns > 0 ? "RIGHT" : "LEFT",std::abs(turns));
        line(label,58,titleFont,uncertain ? amber : green);
        line(!counter.Tracked() ? "Tracking paused - turn estimate incomplete" : uncertain ?
            "Tracking gap - untwist cable, then reset zero" : "Physical turns since zero - reset only when untwisted",
            124,bodyFont,uncertain ? amber : RGB(204,218,221));
        GdiFlush();
        for (int i=0;i<Width*Height;++i) pixels[i] |= 0xff000000;
        if (!bgra) {
            rgba.resize(Width*Height);
            for (int i=0;i<Width*Height;++i) { const auto p=pixels[i]; rgba[i]=(p&0xff00ff00)|((p&255)<<16)|((p>>16)&255); }
        }
    }
    static bool Key(int key, int mods) {
        if (key < 8 || key > 254 || !(GetAsyncKeyState(key)&0x8000)) return false;
        int active = ((GetAsyncKeyState(VK_CONTROL)&0x8000)?1:0) |
            ((GetAsyncKeyState(VK_SHIFT)&0x8000)?2:0) | ((GetAsyncKeyState(VK_MENU)&0x8000)?4:0);
        return active == (mods&7);
    }
};

CableTrackingOverlay::CableTrackingOverlay() : impl(std::make_unique<Impl>()) {}
CableTrackingOverlay::~CableTrackingOverlay() = default;
void CableTrackingOverlay::ReferenceChange(XrTime time) {
    impl->changes.push_back(time);
    std::sort(impl->changes.begin(),impl->changes.end());
}
const XrCompositionLayerBaseHeader* CableTrackingOverlay::Update(XrSession session, XrSpace view,
    XrTime time, bool focused, bool canDraw, ID3D11Device* device, const ocu_cable::Settings& config)
{
    if (impl->session && impl->session != session) impl = std::make_unique<Impl>();
    auto& s = *impl;
    s.session = session;
    if (!session || !config.enabled) return nullptr;
    if (!s.physical && !s.failed) {
        XrReferenceSpaceCreateInfo create{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        create.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; create.poseInReferenceSpace.orientation.w = 1;
        if (xrCreateReferenceSpace(session,&create,&s.physical) != XR_SUCCESS) s.failed = true;
    }
    while (!s.changes.empty() && time >= s.changes.front()) { s.counter.Rebase(); s.changes.erase(s.changes.begin()); }
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const auto flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    bool tracked = focused && s.physical && view && time > 0 &&
        xrLocateSpace(view,s.physical,time,&location) == XR_SUCCESS && (location.locationFlags&flags) == flags;
    const auto q = location.pose.orientation;
    s.counter.Sample(q.x,q.y,q.z,q.w,double(time)*1e-9,tracked);
    const unsigned commands = ocu_cable::requests.exchange(0,std::memory_order_relaxed);
    const bool show = focused && Impl::Key(config.showKey,config.showModifiers);
    const bool reset = focused && Impl::Key(config.resetKey,config.resetModifiers);
    const auto now = GetTickCount64();
    if (focused && ((commands&2) || (reset&&!s.resetHeld))) { s.counter.Reset(); s.warned=false; s.visibleUntil=now+5000; s.nextUpload=0; }
    if (focused && ((commands&1) || (show&&!s.showHeld))) { s.visibleUntil=now+1000*std::clamp(config.displaySeconds,1,30); s.nextUpload=0; }
    s.showHeld=show; s.resetHeld=reset;
    const float threshold = std::isfinite(config.warningTurns) ? std::clamp(config.warningTurns,0.f,20.f) : 0.f;
    if (threshold > 0) {
        if (std::abs(s.counter.Turns()) < threshold*.8) s.warned=false;
        if (focused && !s.warned && std::abs(s.counter.Turns()) >= threshold) {
            s.warned=true; s.visibleUntil=now+1000*std::clamp(config.displaySeconds,1,30); s.nextUpload=0;
        }
    }
    if (!focused || !canDraw || now >= s.visibleUntil || s.failed) return nullptr;
    if (!s.chain && !s.Initialize(device)) { s.failed=true; return nullptr; }
    if (now >= s.nextUpload || !s.uploaded) {
        if (!s.acquired) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (xrAcquireSwapchainImage(s.chain,&acquire,&s.index) != XR_SUCCESS) { s.failed=true; return nullptr; }
            s.acquired=true;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wait.timeout=0;
        const auto result = xrWaitSwapchainImage(s.chain,&wait);
        if (result == XR_TIMEOUT_EXPIRED) return nullptr; // retry this acquired image, never reacquire it
        if (result != XR_SUCCESS || s.index >= s.images.size() || !s.images[s.index].texture) { s.failed=true; return nullptr; }
        s.Rasterize();
        s.context->UpdateSubresource(s.images[s.index].texture,0,nullptr,s.bgra?s.pixels:s.rgba.data(),Impl::Width*4,0);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (xrReleaseSwapchainImage(s.chain,&release) != XR_SUCCESS) { s.failed=true; return nullptr; }
        s.acquired=false; s.uploaded=true; s.nextUpload=now+200;
    }
    s.quad.space=view;
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&s.quad);
}
#endif
