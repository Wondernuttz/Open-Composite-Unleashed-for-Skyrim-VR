#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "OpenOVR/Compositor/DensityMaskManager.h"

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*, long, const char*, const char* format, ...) {
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args); std::puts("");
}
static unsigned checks;
static void Require(bool ok, const char* text) { ++checks; if (!ok) throw std::runtime_error(text); }
static void HR(HRESULT result) {
    if (FAILED(result)) { std::printf("HRESULT %08X\n", unsigned(result)); throw std::runtime_error("D3D11 failure"); }
}
static std::vector<unsigned char> Read(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture, unsigned stride)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE map{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map));
    std::vector<unsigned char> result(size_t(desc.Width) * desc.Height * stride);
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(result.data() + size_t(y) * desc.Width * stride,
            static_cast<unsigned char*>(map.pData) + y * map.RowPitch, desc.Width * stride);
    context->Unmap(staging.Get(), 0); return result;
}

struct Fixture {
    static constexpr unsigned Width = 272, Height = 138, EyeWidth = Width / 2;
    static constexpr unsigned Columns = (EyeWidth + 7) / 8, Rows = (Height + 7) / 8;
    static constexpr unsigned char OmittedCoverage = 64;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> debug;
    DensityMaskManager manager;
    DensityMaskManager::PatternSettings pattern{.2f, .65f, false, true,
        {ocu_foveation::Rate::X1x1, ocu_foveation::Rate::X2x2, ocu_foveation::Rate::X4x2}};
    float centers[2][2]{{.45f, .52f}, {.55f, .48f}};
    ComPtr<ID3D11Texture2D> depth, privateDepth, coverage, eligibility;
    ComPtr<ID3D11DepthStencilView> depthView, privateView;
    ComPtr<ID3D11RenderTargetView> coverageView;
    ComPtr<ID3D11ShaderResourceView> coverageRead, eligibilityRead;
    std::array<ComPtr<ID3D11Texture2D>, 8> colors;
    std::array<ComPtr<ID3D11RenderTargetView>, 8> views;
    std::array<UINT8, 8> writeMasks;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11DepthStencilState> equalDepth;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11BlendState> blend;
    std::array<std::vector<unsigned char>, 8> reference;
    std::array<std::vector<unsigned char>, 8> initial;
    std::vector<unsigned char> originalDepth;

    ComPtr<ID3D11Texture2D> Texture(unsigned width, unsigned height, DXGI_FORMAT format,
        unsigned flags, const void* data = nullptr, unsigned stride = 4)
    {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1; desc.Format = format; desc.BindFlags = flags;
        D3D11_SUBRESOURCE_DATA source{data, width * stride, 0}; ComPtr<ID3D11Texture2D> result;
        HR(device->CreateTexture2D(&desc, data ? &source : nullptr, &result)); return result;
    }
    static bool Eligible(unsigned x, unsigned y) {
        return ((x + 1) * 8 <= EyeWidth) && ((y + 1) * 8 <= Height) && !(x % 7 == 0 && y % 5 == 0);
    }
    Fixture(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL level, bool partialWrites) {
        HR(D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP,
            nullptr, D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
        HR(device.As(&debug)); Require(manager.Initialize(device.Get()), "initialize production manager");
        for (unsigned slot = 0; slot < 8; ++slot) {
            colors[slot] = Texture(Width, Height, DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
            HR(device->CreateRenderTargetView(colors[slot].Get(), nullptr, &views[slot]));
            writeMasks[slot] = partialWrites && slot % 2 ?
                D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_BLUE : D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        depth = Texture(Width, Height, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
        privateDepth = Texture(Width, Height, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        HR(device->CreateDepthStencilView(depth.Get(), &dv, &depthView));
        HR(device->CreateDepthStencilView(privateDepth.Get(), &dv, &privateView));
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .5f, 0x5a);
        originalDepth = Read(device.Get(), context.Get(), depth.Get(), 4);
        coverage = Texture(Width, Height, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        HR(device->CreateRenderTargetView(coverage.Get(), nullptr, &coverageView));
        HR(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageRead));
        std::vector<float> eligible(Columns * 2 * Rows);
        for (unsigned y = 0; y < Rows; ++y) for (unsigned x = 0; x < Columns * 2; ++x)
            eligible[y * Columns * 2 + x] = Eligible(x % Columns, y) ? 1.f : 0.f;
        eligibility = Texture(Columns * 2, Rows, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, eligible.data());
        HR(device->CreateShaderResourceView(eligibility.Get(), nullptr, &eligibilityRead));
        constexpr char shader[] = R"HLSL(
float4 VS(uint id:SV_VertexID):SV_Position { return float4(id==2?3:-1,id==1?-3:1,.5,1); }
struct O { float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3;
    float4 e:SV_Target4;float4 f:SV_Target5;float4 g:SV_Target6;float4 h:SV_Target7; };
[earlydepthstencil] O PS(float4 p:SV_Position) {
    uint eye=uint(p.x)>=136;uint2 cluster=uint2((uint(p.x)-eye*136)/8,uint(p.y)/8);
    float v=.2+float((cluster.x*13+cluster.y*7+eye*3)%101)/202.;O o;
    o.a=float4(v,.4,.7,1);o.b=float4(.2,v,.5,1);o.c=float4(.6,.1,v,1);o.d=float4(v,.3,.6,1);
    o.e=float4(.7,v,.2,1);o.f=float4(.1,.8,v,1);o.g=float4(v,.9,.4,1);o.h=float4(.4,v,.8,1);return o;
}
)HLSL";
        auto compile = [&](const char* entry, const char* target) {
            ComPtr<ID3DBlob> code, errors; HRESULT hr = D3DCompile(shader, sizeof(shader) - 1, "RDM blackout test",
                nullptr, nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer())); HR(hr); return code;
        };
        auto code = compile("VS", "vs_5_0"); HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
        code = compile("PS", "ps_5_0"); HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps));
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthFunc = D3D11_COMPARISON_EQUAL;
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; HR(device->CreateDepthStencilState(&ds, &equalDepth));
        D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE; HR(device->CreateRasterizerState(&rs, &raster));
        D3D11_BLEND_DESC bs{}; bs.IndependentBlendEnable = TRUE;
        for (unsigned i = 0; i < 8; ++i) bs.RenderTarget[i].RenderTargetWriteMask = writeMasks[i];
        HR(device->CreateBlendState(&bs, &blend));
        Clear(); for (unsigned i = 0; i < 8; ++i) initial[i] = Read(device.Get(), context.Get(), colors[i].Get(), 4);
        Run(false); for (unsigned i = 0; i < 8; ++i) reference[i] = Read(device.Get(), context.Get(), colors[i].Get(), 4);
        debug->ClearStoredMessages();
    }
    void Clear() {
        context->ClearState(); const float color[4]{.07f, .13f, .19f, .23f}, zero[4]{};
        for (auto& view : views) context->ClearRenderTargetView(view.Get(), color);
        context->ClearRenderTargetView(coverageView.Get(), zero);
    }
    void Bind(ID3D11DepthStencilView* depthTarget) {
        ID3D11RenderTargetView* targets[8]; for (unsigned i = 0; i < 8; ++i) targets[i] = views[i].Get();
        context->OMSetRenderTargets(8, targets, depthTarget); context->OMSetDepthStencilState(equalDepth.Get(), 0);
        context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu); context->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1}; context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    }
    void Run(bool rdm, bool supplyEligibility = true) {
        Clear(); manager.SetPatternSettings(pattern);
        manager.SetProjectionCenters(centers[0][0], centers[0][1], centers[1][0], centers[1][1]);
        ID3D11Texture2D* targets[8]; for (unsigned i = 0; i < 8; ++i) targets[i] = colors[i].Get();
        Require(manager.PrepareMRTTargets(targets, 8, Width, Height, {0, 0, EyeWidth, Height},
            {EyeWidth, 0, EyeWidth, Height}, writeMasks.data()), "prepare packed targets");
        Bind(depthView.Get());
        if (rdm) {
            context->CopyResource(privateDepth.Get(), depth.Get());
            Require(manager.ApplyDepthMask(privateView.Get(), 1.f, coverageView.Get(),
                supplyEligibility ? eligibilityRead.Get() : nullptr), "apply private mask");
        }
        Bind(rdm ? privateView.Get() : depthView.Get()); context->Draw(3, 0);
        if (rdm) { Bind(depthView.Get()); Require(manager.ResolveMRTs(coverageRead.Get(), 3), "resolve packed MRTs"); }
    }
    unsigned Verify(bool expectOmissions, bool supplyEligibility = true) {
        Run(true, supplyEligibility); auto mask = Read(device.Get(), context.Get(), coverage.Get(), 1);
        unsigned omitted = 0;
        for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
            const unsigned eye = x / EyeWidth, localX = x % EyeWidth; auto pixel = size_t(y) * Width + x;
            const bool culled = mask[pixel] == OmittedCoverage;
            Require(mask[pixel] == 0 || mask[pixel] == 255 || culled, "unexpected coverage encoding");
            if (culled) {
                ++omitted; Require(expectOmissions && supplyEligibility && Eligible(localX / 8, y / 8), "culled protected cluster");
                Require(ocu_foveation::WholeBlackoutTile(pattern.blackout, centers[eye][0], centers[eye][1],
                    pattern.innerRadius, pattern.midRadius, pattern.horizontalScale, (localX / 8) * 8, (y / 8) * 8,
                    8, 8, EyeWidth, Height), "GPU omission escapes padded shared geometry proof");
            }
        }
        for (unsigned i = 0; i < 8; ++i) {
            auto actual = Read(device.Get(), context.Get(), colors[i].Get(), 4);
            for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
                auto& expected = mask[pixel] == OmittedCoverage ? initial[i] : reference[i];
                Require(std::memcmp(actual.data() + pixel * 4, expected.data() + pixel * 4, 4) == 0,
                    "visible MRT changed or omitted pixel received stale reconstruction");
            }
        }
        Require(Read(device.Get(), context.Get(), depth.Get(), 4) == originalDepth, "original depth/stencil changed");
        ID3D11RenderTargetView* targets[8]{}; ComPtr<ID3D11DepthStencilView> boundDepth;
        context->OMGetRenderTargets(8, targets, &boundDepth); Require(boundDepth.Get() == depthView.Get(), "depth binding not restored");
        for (unsigned i = 0; i < 8; ++i) { Require(targets[i] == views[i].Get(), "MRT binding not restored"); if (targets[i]) targets[i]->Release(); }
        return omitted;
    }
    void DebugCheck(bool depthOnlyMask = false) {
        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0; debug->GetMessage(i, nullptr, &bytes); std::vector<char> storage(bytes);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); HR(debug->GetMessage(i, message, &bytes));
            // The public depth-only mask path intentionally discards its color
            // output. Permit only this exact diagnostic around that one case.
            if (depthOnlyMask && message->ID == D3D11_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET) continue;
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::puts(message->pDescription); throw std::runtime_error("D3D11 warning/error");
            }
        }
        debug->ClearStoredMessages();
    }
};

static void Run(IDXGIAdapter* adapter, D3D_FEATURE_LEVEL level, bool partialWrites) {
    Fixture f(adapter, level, partialWrites); unsigned omissions = 0, cases = 0;
    for (float scale : {.5f, 1.f, 2.f}) for (unsigned flags = 0; flags < 8; ++flags) {
        f.pattern.horizontalScale = scale;
        f.pattern.blackout = {bool(flags & 1), bool(flags & 2), bool(flags & 4), .9f, 0.f};
        const unsigned unguarded = f.Verify(flags != 0); omissions += unguarded; ++cases;
        Require(flags == 0 ? unguarded == 0 : unguarded > 0, "selected blackout region never omitted pixels");
        f.pattern.blackout.guardPixels = 8.f; const unsigned guarded = f.Verify(flags != 0);
        Require(guarded <= unguarded, "larger guard band increased omissions"); omissions += guarded; ++cases;
    }
    // Touching middle/cutoff ranges must merge; a separated cutoff leaves a visible band.
    f.pattern.horizontalScale = 1.f; f.pattern.blackout = {true, false, true, .65f, 0.f};
    const unsigned joined = f.Verify(true); f.pattern.blackout.cutoffRadius = .95f;
    Require(f.Verify(true) < joined, "middle/cutoff gap was incorrectly culled");
    f.pattern.blackout = {true, true, true, .1f, 0.f}; f.Verify(true);
    f.pattern.blackout.guardPixels = 1000.f; Require(f.Verify(true) == 0, "guard band was silently truncated");
    f.pattern.blackout.guardPixels = 0; Require(f.Verify(false, false) == 0, "omissions allowed without eligibility guide");
    f.pattern.blackout.guardPixels = std::numeric_limits<float>::quiet_NaN(); f.Verify(false);
    f.pattern.blackout.guardPixels = -1.f; f.Verify(false);
    f.pattern.blackout.guardPixels = 0.f;
    f.pattern.blackout.cutoffRadius = std::numeric_limits<float>::quiet_NaN(); f.Verify(false);
    f.pattern.blackout.cutoffRadius = .9f;
    f.centers[0][0] = -.1f; f.centers[1][0] = 1.1f; f.Verify(false);
    f.centers[0][0] = .45f; f.centers[1][0] = .55f;
    // Without a coverage RTV the caller cannot distinguish omitted pixels from
    // reconstructable holes. Compare private depth directly against no blackout.
    f.Run(true); f.DebugCheck(); f.context->OMSetRenderTargets(0, nullptr, nullptr);
    f.context->CopyResource(f.privateDepth.Get(), f.depth.Get());
    Require(f.manager.ApplyDepthMask(f.privateView.Get(), 1.f, nullptr, f.eligibilityRead.Get()), "mask without coverage");
    auto noCoverage = Read(f.device.Get(), f.context.Get(), f.privateDepth.Get(), 4);
    f.pattern.blackout = {}; f.manager.SetPatternSettings(f.pattern);
    f.context->CopyResource(f.privateDepth.Get(), f.depth.Get());
    Require(f.manager.ApplyDepthMask(f.privateView.Get(), 1.f, nullptr, f.eligibilityRead.Get()), "disabled mask without coverage");
    Require(Read(f.device.Get(), f.context.Get(), f.privateDepth.Get(), 4) == noCoverage,
        "omissions allowed without coverage record");
    f.DebugCheck(true);
    f.pattern.blackout = {}; Require(f.Verify(false) == 0, "disable retained omission history");
    f.DebugCheck(); Require(omissions != 0, "no blackout cases culled pixels");
    std::printf("RDM blackout PASS: %s FL %x partialWrites=%d cases=%u all mask unions/aspects, guard bands, ownership rejection, partial tiles, eight MRTs, depth/state\n",
        adapter ? "hardware" : "WARP", unsigned(level), int(partialWrites), cases);
}
int main(int argc, char** argv) {
    try {
        const bool warpOnly = argc > 1 && !std::strcmp(argv[1], "--warp");
        Run(nullptr, D3D_FEATURE_LEVEL_11_0, false); Run(nullptr, D3D_FEATURE_LEVEL_11_0, true);
        if (!warpOnly) {
            ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory))); unsigned hardware = 0;
            for (unsigned i = 0;; ++i) {
                ComPtr<IDXGIAdapter1> adapter; const auto result = factory->EnumAdapters1(i, &adapter);
                if (result == DXGI_ERROR_NOT_FOUND) break; HR(result);
                DXGI_ADAPTER_DESC1 desc{}; HR(adapter->GetDesc1(&desc)); if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                std::printf("RDM blackout hardware: %ls\n", desc.Description);
                Run(adapter.Get(), D3D_FEATURE_LEVEL_11_0, false); Run(adapter.Get(), D3D_FEATURE_LEVEL_11_1, true); ++hardware;
            }
            Require(hardware != 0, "no hardware adapters tested");
        }
        std::printf("PASS %u RDM blackout checks\n", checks); return 0;
    } catch (const std::exception& e) { std::printf("FAIL after %u checks: %s\n", checks, e.what()); return 1; }
}
