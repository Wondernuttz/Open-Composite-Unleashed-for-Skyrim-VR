#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cmath>
#include <limits>
#include <algorithm>
#include "OpenOVR/Compositor/DensityMaskManager.h"

using Microsoft::WRL::ComPtr;
// Test sink; the production manager and its actual HLSL are compiled unchanged.
void oovr_log_raw(const char*, long, const char*, const char* msg) { std::puts(msg); }
void oovr_log_raw_format(const char*, long, const char*, const char* fmt, ...)
{
    va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts("");
}
static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
static void HR(HRESULT hr) { Require(SUCCEEDED(hr), "D3D11 call failed"); }

static std::vector<unsigned char> Read(ID3D11Device* dev, ID3D11DeviceContext* ctx,
    ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging; HR(dev->CreateTexture2D(&desc, nullptr, &staging));
    ctx->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{}; HR(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<unsigned char> result(desc.Width * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(result.data() + y * desc.Width * 4,
            static_cast<unsigned char*>(map.pData) + y * map.RowPitch, desc.Width * 4);
    ctx->Unmap(staging.Get(), 0); return result;
}

static void Run(D3D_DRIVER_TYPE driver, UINT width, UINT height, IDXGIAdapter* adapter = nullptr)
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level{};
    HR(D3D11CreateDevice(adapter, driver, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &dev, &level, &ctx));
    DensityMaskManager manager;
    Require(manager.Initialize(dev.Get()), "production shader initialization failed");
    const UINT eyeWidth = width / 2;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color; HR(dev->CreateTexture2D(&td, nullptr, &color));
    ComPtr<ID3D11RenderTargetView> colorRTV;
    HR(dev->CreateRenderTargetView(color.Get(), nullptr, &colorRTV));
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> auxiliary; HR(dev->CreateTexture2D(&td, nullptr, &auxiliary));
    ComPtr<ID3D11UnorderedAccessView> auxiliaryUAV;
    HR(dev->CreateUnorderedAccessView(auxiliary.Get(), nullptr, &auxiliaryUAV));
    td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth; HR(dev->CreateTexture2D(&td, nullptr, &depth));
    ComPtr<ID3D11DepthStencilView> dsv; HR(dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv));

    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = 0;
    ComPtr<ID3D11BlendState> hostileBlend; HR(dev->CreateBlendState(&bd, &hostileBlend));
    const std::array<float, 4> hostileConstants{20.0f, 0, 0, 0};
    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(hostileConstants);
    cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = hostileConstants.data();
    ComPtr<ID3D11Buffer> hostileCB; HR(dev->CreateBuffer(&cbd, &initial, &hostileCB));
    ID3D11Buffer* cb = hostileCB.Get();

    static_assert(DensityMaskManager::PatternSettings{}.horizontalScale == 1.0f,
        "Existing aggregate callers must retain the original ring geometry");
    for (float horizontalScale : {.5f, 1.f, 1.3f, 2.f, 0.f, 4.f,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    for (int pattern = -1; pattern < 14; ++pattern)
    for (bool compatibility : {true, false}) for (float clearDepth : {1.0f, 0.0f}) {
        using namespace ocu_foveation;
        bool custom = pattern >= 0;
        const Rate rate = static_cast<Rate>(custom ? pattern % 7 : 0);
        RingRates requested = pattern < 7 ? RingRates{rate, rate, rate} :
            RingRates{rate, static_cast<Rate>((pattern + 2) % 7), static_cast<Rate>((pattern + 4) % 7)};
        RingRates effective = ResolveRates(true, custom, compatibility, true, requested);
        manager.BeginFrame();
        manager.SetPatternSettings({0.4f, 0.7f, compatibility, custom, effective, horizontalScale});
        const float effectiveScale = std::isfinite(horizontalScale) ? std::clamp(horizontalScale, .5f, 2.f) : 1.f;
        manager.SetProjectionCenters(0.45f, 0.55f, 0.6f, 0.4f);
        Require(manager.PrepareStereoTarget(color.Get(), width, height,
            {0, 0, static_cast<int>(eyeWidth), static_cast<int>(height)},
            {static_cast<int>(eyeWidth), 0, static_cast<int>(eyeWidth), static_cast<int>(height)}),
            "prepare stereo target failed");
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, clearDepth, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        const float factors[4] = {0.1f, 0.2f, 0.3f, 0.4f};
        ctx->OMSetBlendState(hostileBlend.Get(), factors, 0);
        auto* appRTV = colorRTV.Get(); auto* appUAV = auxiliaryUAV.Get();
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &appRTV, dsv.Get(), 1, 1, &appUAV, nullptr);
        Require(manager.ApplyDepthMask(dsv.Get(), clearDepth), "mask draw failed");
        ComPtr<ID3D11UnorderedAccessView> restoredUAV;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &restoredUAV);
        Require(restoredUAV.Get() == auxiliaryUAV.Get(), "mask discarded application OM UAV");
        auto depthBytes = Read(dev.Get(), ctx.Get(), depth.Get());
        int masked[2]{}, survivors[2]{};
        std::vector<unsigned char> pixels(width * height * 4);
        std::vector<UINT> sampleOffsets(width * height);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            float value; memcpy(&value, depthBytes.data() + (y * width + x) * 4, 4);
            const int eye = x >= eyeWidth;
            bool survived = value == clearDepth;
            const UINT localX = x - eye * eyeWidth;
            UINT donorX = x, donorY = y;
            if (custom && (localX / 8 + 1) * 8 <= eyeWidth && (y / 8 + 1) * 8 <= height) {
                float dx = (localX / 8) * (8.0f / eyeWidth) - (eye ? 0.6f : 0.45f);
                float dy = (y / 8) * (8.0f / height) - (eye ? 0.4f : 0.55f);
                dx *= 1.f / effectiveScale;
                float distance = 2 * std::sqrt(dx * dx + dy * dy);
                const auto size = Dimensions(distance < 0.4f ? effective.inner :
                    (distance < 0.7f ? effective.mid : effective.outer));
                const UINT offsetX = ((localX / 2) % size.x) * 2;
                const UINT offsetY = ((y / 2) % size.y) * 2;
                Require(survived == (offsetX == 0 && offsetY == 0), "custom shader density/axis/ring differs from selected rate");
                donorX -= offsetX; donorY -= offsetY;
            } else if (custom) Require(survived, "partial edge cluster must remain full detail");
            sampleOffsets[y * width + x] = donorY * width + donorX;
            Require(survived || value == 1.0f - clearDepth, "invalid mask depth");
            (survived ? survivors[eye] : masked[eye])++;
            auto* p = pixels.data() + (y * width + x) * 4;
            p[0] = survived && !eye ? 220 : 0;
            p[1] = survived && eye ? 180 : 0;
            p[2] = survived ? (custom ? 1 + ((x * 13 + y * 7) % 250) : 70) : 0;
            p[3] = survived ? 64 : 0;
        }
        for (int eye = 0; eye < 2; ++eye)
            Require((custom || masked[eye] > 0) && survivors[eye] > 0, "mask did not write both eyes");
        ctx->UpdateSubresource(color.Get(), 0, nullptr, pixels.data(), width * 4, 0);
        // Re-establish hostile application state immediately before reconstruction.
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->OMSetBlendState(hostileBlend.Get(), factors, 0);
        auto* output = manager.ReconstructStereo(color.Get(), 0);
        Require(output != nullptr, "reconstruction failed");
        restoredUAV.Reset();
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 1, 1, &restoredUAV);
        Require(restoredUAV.Get() == auxiliaryUAV.Get(), "reconstruction discarded application OM UAV");
        ComPtr<ID3D11BlendState> restoredBlend; float restoredFactors[4]{}; UINT restoredMask = 1;
        ctx->OMGetBlendState(&restoredBlend, restoredFactors, &restoredMask);
        Require(restoredBlend.Get() == hostileBlend.Get() && restoredMask == 0 &&
            restoredFactors[2] == factors[2], "application blend state was not restored");
        ComPtr<ID3D11Buffer> restoredCB; ctx->VSGetConstantBuffers(0, 1, &restoredCB);
        Require(restoredCB.Get() == hostileCB.Get(), "application VS constants were not restored");
        auto actual = Read(dev.Get(), ctx.Get(), output);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
            const auto* p = actual.data() + (y * width + x) * 4;
            const bool eye = x >= eyeWidth;
            if (!(p[0] == (eye ? 0 : 220) && p[1] == (eye ? 180 : 0) &&
                p[2] == (custom ? pixels[sampleOffsets[y * width + x] * 4 + 2] : 70) && p[3] == 64))
                std::printf("reconstruction mismatch: dimensions=%ux%u aspect=%g pattern=%d compatibility=%d clear=%g pixel=%u,%u donor=%u actual=%u,%u,%u,%u expectedBlue=%u\n",
                    width,height,horizontalScale,pattern,compatibility,clearDepth,x,y,sampleOffsets[y*width+x],
                    p[0],p[1],p[2],p[3],custom?pixels[sampleOffsets[y*width+x]*4+2]:70);
            Require(p[0] == (eye ? 0 : 220) && p[1] == (eye ? 180 : 0) &&
                p[2] == (custom ? pixels[sampleOffsets[y * width + x] * 4 + 2] : 70) && p[3] == 64,
                "pixel reconstruction failed, sampled masked pixels or crossed eyes");
        }
        manager.EndFrameMasking();
        Require(manager.ReconstructStereo(color.Get(), 1) != nullptr, "second-eye submit lost current frame");
        manager.BeginFrame(); // Next frame may immediately return due to menu/gaze loss.
        Require(!manager.IsArmed() && !manager.WasMaskAppliedThisFrame() &&
            manager.ReconstructStereo(color.Get(), 0) == nullptr,
            "previous frame mask survived frame boundary");
    }
    std::printf("DensityMask D3D11 %s PASS: horizontal scale .5/1/1.3/2, finite clamp and nonfinite fallback, CPU mask/donor parity\n",
        driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
}

int main()
{
    try {
        Run(D3D_DRIVER_TYPE_WARP, 128, 64);
        Run(D3D_DRIVER_TYPE_WARP, 134, 70);
        Run(D3D_DRIVER_TYPE_WARP, 130, 66);
        ComPtr<IDXGIFactory1> factory;
        HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        unsigned hardwareCount = 0;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; HR(adapter->GetDesc1(&desc));
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            std::printf("Testing RDM GPU: %ls vendor=0x%04X\n", desc.Description, desc.VendorId);
            Run(D3D_DRIVER_TYPE_UNKNOWN, 128, 64, adapter.Get());
            Run(D3D_DRIVER_TYPE_UNKNOWN, 134, 70, adapter.Get());
            Run(D3D_DRIVER_TYPE_UNKNOWN, 130, 66, adapter.Get());
            ++hardwareCount;
        }
        Require(hardwareCount > 0, "no hardware adapters tested");
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return 0;
}
