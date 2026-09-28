// Live WARP tests of the production menu capture and final composition module.
#include "OpenOVR/Compositor/MenuLayerRenderer.h"
#include "OpenOVR/Compositor/SkyrimMenuTargets.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/MenuShaderMetadata.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*, long, const char*, const char* format, ...)
{
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args); std::puts("");
}
namespace {
unsigned checks = 0;
void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
void HR(HRESULT value) { Check(SUCCEEDED(value), "D3D11 operation failed"); }
struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
Target Color(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target result;
    HR(device->CreateTexture2D(&desc, nullptr, &result.texture));
    HR(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv));
    HR(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv));
    return result;
}
struct Pixel { float r, g, b, a; };
std::vector<Pixel> Read(ID3D11Device* device, ID3D11DeviceContext* context, const Target& target)
{
    D3D11_TEXTURE2D_DESC desc{};
    target.texture->GetDesc(&desc);
    Check(desc.Format == DXGI_FORMAT_R32G32B32A32_FLOAT, "readback target format");
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HR(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), target.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<Pixel> pixels(size_t(desc.Width) * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.data() + size_t(y) * desc.Width,
            static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch, desc.Width * sizeof(Pixel));
    context->Unmap(staging.Get(), 0);
    return pixels;
}
ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> bytecode, error;
    const HRESULT result = D3DCompile(source, std::strlen(source), "menu tests", nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &bytecode, &error);
    if (FAILED(result) && error) std::printf("%s\n", static_cast<const char*>(error->GetBufferPointer()));
    HR(result);
    return bytecode;
}
bool Near(float a, float b) { return std::fabs(a - b) < .004f; }
void PixelIs(const Pixel& value, const Pixel& expected, const char* message)
{
    Check(Near(value.r, expected.r) && Near(value.g, expected.g) && Near(value.b, expected.b) && Near(value.a, expected.a), message);
}

// Record graphics and compute bindings, including class instances and D3D11.1
// constant-buffer ranges. Get calls are balanced immediately; this does not
// modify the pipeline or hold its resources alive on behalf of the module.
struct Pipeline {
    std::vector<unsigned char> bytes;
    template<class T> void Add(const T& value) {
        const auto* data = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(value));
    }
    template<class T> void Object(T* object) {
        Add(reinterpret_cast<std::uintptr_t>(object));
        if (object) object->Release();
    }
    explicit Pipeline(ID3D11DeviceContext* c) {
        ComPtr<ID3D11DeviceContext1> c1; HR(c->QueryInterface(IID_PPV_ARGS(&c1)));
        ID3D11InputLayout* input = nullptr; c->IAGetInputLayout(&input); Object(input);
        D3D11_PRIMITIVE_TOPOLOGY topology{}; c->IAGetPrimitiveTopology(&topology); Add(topology);
        ID3D11Buffer* vertices[32]{}; UINT strides[32]{}, offsets[32]{};
        c->IAGetVertexBuffers(0, 32, vertices, strides, offsets);
        for (auto* value : vertices) Object(value); Add(strides); Add(offsets);
        ID3D11Buffer* index = nullptr; DXGI_FORMAT format{}; UINT offset{};
        c->IAGetIndexBuffer(&index, &format, &offset); Object(index); Add(format); Add(offset);
#define STAGE(Name, Type) \
        { Type* shader = nullptr; ID3D11ClassInstance* instances[256]{}; UINT count = 256; \
          c->Name##GetShader(&shader, instances, &count); Object(shader); Add(count); \
          for (UINT i = 0; i < count; ++i) Object(instances[i]); \
          ID3D11Buffer* buffers[14]{}; UINT first[14]{}, length[14]{}; \
          c1->Name##GetConstantBuffers1(0, 14, buffers, first, length); \
          for (auto* value : buffers) Object(value); Add(first); Add(length); \
          ID3D11ShaderResourceView* resources[128]{}; c->Name##GetShaderResources(0, 128, resources); \
          for (auto* value : resources) Object(value); \
          ID3D11SamplerState* samplers[16]{}; c->Name##GetSamplers(0, 16, samplers); \
          for (auto* value : samplers) Object(value); }
        STAGE(VS, ID3D11VertexShader); STAGE(HS, ID3D11HullShader); STAGE(DS, ID3D11DomainShader);
        STAGE(GS, ID3D11GeometryShader); STAGE(PS, ID3D11PixelShader); STAGE(CS, ID3D11ComputeShader);
#undef STAGE
        ID3D11RenderTargetView* targets[8]{}; ID3D11DepthStencilView* dsv = nullptr;
        c->OMGetRenderTargets(8, targets, &dsv); for (auto* value : targets) Object(value); Object(dsv);
        ID3D11UnorderedAccessView* uavs[8]{};
        c->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 8, uavs);
        for (auto* value : uavs) Object(value);
        c->CSGetUnorderedAccessViews(0, 8, uavs); for (auto* value : uavs) Object(value);
        ID3D11Buffer* stream[4]{}; c->SOGetTargets(4, stream); for (auto* value : stream) Object(value);
        ID3D11BlendState* blend = nullptr; FLOAT factor[4]{}; UINT mask{};
        c->OMGetBlendState(&blend, factor, &mask); Object(blend); Add(factor); Add(mask);
        ID3D11DepthStencilState* depth = nullptr; UINT stencil{};
        c->OMGetDepthStencilState(&depth, &stencil); Object(depth); Add(stencil);
        ID3D11RasterizerState* raster = nullptr; c->RSGetState(&raster); Object(raster);
        UINT count = 16; D3D11_VIEWPORT viewports[16]{}; c->RSGetViewports(&count, viewports);
        Add(count); for (UINT i = 0; i < count; ++i) Add(viewports[i]);
        count = 16; D3D11_RECT scissors[16]{}; c->RSGetScissorRects(&count, scissors);
        Add(count); for (UINT i = 0; i < count; ++i) Add(scissors[i]);
        ID3D11Predicate* predicate = nullptr; BOOL predicateValue{};
        c->GetPredication(&predicate, &predicateValue); Object(predicate); Add(predicateValue);
    }
};

constexpr char Shaders[] = R"HLSL(
Texture2D<float4> source : register(t0);
SamplerState pointSampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex result; result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
    return result;
}
float4 Menu(Vertex input) : SV_Target { return source.Sample(pointSampler, input.uv); }
float4 Premultiplied(Vertex input) : SV_Target {
    float4 color = source.Sample(pointSampler, input.uv); return float4(color.rgb * color.a, color.a);
}
float4 ProcessScene(Vertex input) : SV_Target {
    float4 color = source.Sample(pointSampler, input.uv);
    return float4(lerp(color.rgb, float3(0.2, 0.2, 0.2), 0.9), 1);
}
float4 PositionBased(Vertex input) : SV_Target { return source.Load(int3(input.position.xy, 0)); }
)HLSL";

struct DrawToken { ID3D11DeviceContext* context; unsigned calls = 0; };
void NativeDraw(void* raw) { auto& draw = *static_cast<DrawToken*>(raw); ++draw.calls; draw.context->Draw(3, 0); }

struct Fixture {
    static constexpr UINT renderW = 64, renderH = 32, outputW = 128, outputH = 64;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3D11InfoQueue> info;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> menu, premultiplied, process, positionBased;
    ComPtr<ID3D11SamplerState> point;
    ComPtr<ID3D11BlendState> straight, pre, additive;
    ComPtr<ID3D11DepthStencilState> noDepth, alwaysDepth, writeDepth;
    ComPtr<ID3D11RasterizerState> raster, scissorRaster;
    Target source, native, other, scene, left, right;
    MenuLayerRenderer renderer;
    std::uint64_t frame = 0;
    std::vector<Pixel> sourcePixels;

    explicit Fixture(bool debug) {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
        HR(context.As(&context1)); device.As(&info);
        Check(ocu_vrs_guard::InstallShaderCapture(device.Get()), "production shader metadata hook installed");
        auto bytes = Compile(Shaders, "VS", "vs_5_0");
        HR(device->CreateVertexShader(bytes->GetBufferPointer(), bytes->GetBufferSize(), nullptr, &vs));
        auto makePS = [&](const char* entry, ComPtr<ID3D11PixelShader>& shader) {
            auto code = Compile(Shaders, entry, "ps_5_0");
            HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
        };
        makePS("Menu", menu); makePS("Premultiplied", premultiplied);
        makePS("ProcessScene", process); makePS("PositionBased", positionBased);
        Check(ocu_menu::ShaderCanScale(menu.Get()) && ocu_menu::ShaderCanScale(premultiplied.Get()), "sampled menu shader is admitted");
        Check(!ocu_menu::ShaderCanScale(positionBased.Get()), "pixel-coordinate Load shader is rejected");
        D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sample.MaxLOD = D3D11_FLOAT32_MAX; HR(device->CreateSamplerState(&sample, &point));
        D3D11_BLEND_DESC blend{}; auto& b = blend.RenderTarget[0];
        b.BlendEnable = TRUE; b.SrcBlend = D3D11_BLEND_SRC_ALPHA; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD; b.SrcBlendAlpha = D3D11_BLEND_ONE;
        b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HR(device->CreateBlendState(&blend, &straight)); b.SrcBlend = D3D11_BLEND_ONE;
        HR(device->CreateBlendState(&blend, &pre)); b.DestBlend = D3D11_BLEND_ONE;
        HR(device->CreateBlendState(&blend, &additive));
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; HR(device->CreateDepthStencilState(&depth, &noDepth));
        depth.DepthEnable = TRUE; HR(device->CreateDepthStencilState(&depth, &alwaysDepth));
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; HR(device->CreateDepthStencilState(&depth, &writeDepth));
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE; HR(device->CreateRasterizerState(&rd, &raster));
        rd.ScissorEnable = TRUE; HR(device->CreateRasterizerState(&rd, &scissorRaster));
        source = Color(device.Get(), 32, 16); native = Color(device.Get(), renderW, renderH);
        other = Color(device.Get(), renderW, renderH); scene = Color(device.Get(), 64, 64);
        left = Color(device.Get(), 64, 64); right = Color(device.Get(), 64, 64);
        sourcePixels.resize(32 * 16);
        for (UINT y = 0; y < 16; ++y) for (UINT x = 0; x < 32; ++x) {
            const bool stroke = ((x % 8) == 2 || (x % 8) == 3 || (y % 8) == 2);
            const float alpha = stroke ? 1.f : (((x % 8) == 5 && y < 8) ? .5f : 0.f);
            sourcePixels[y * 32 + x] = x < 16 ? Pixel{1, .5f, .25f, alpha} : Pixel{.25f, .5f, 1, alpha};
        }
        context->UpdateSubresource(source.texture.Get(), 0, nullptr, sourcePixels.data(), 32 * sizeof(Pixel), 0);
        const float sceneColor[4]{.7f, .4f, .1f, 1}; context->ClearRenderTargetView(scene.rtv.Get(), sceneColor);
        Check(renderer.Initialize(device.Get(), context.Get()), "renderer initialized");
    }
    ocu_menu::Targets Targets() {
        ocu_menu::Targets result{}; result.context = context.Get();
        result.sources[0] = source.texture.Get(); result.destinations[0] = native.texture.Get(); return result;
    }
    bool Begin(UINT outputWidth = outputW, UINT outputHeight = outputH) {
        return renderer.BeginFrame(++frame, renderW, renderH, outputWidth, outputHeight, Targets());
    }
    void Bind(Target& destination, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps, UINT width, UINT height) {
        context->ClearState();
        context->OMSetRenderTargets(1, destination.rtv.GetAddressOf(), nullptr);
        context->OMSetBlendState(straight.Get(), nullptr, UINT(-1));
        context->OMSetDepthStencilState(noDepth.Get(), 7);
        context->RSSetState(raster.Get());
        const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps, nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv); context->PSSetSamplers(0, 1, point.GetAddressOf());
    }
    void BindMenu() { Bind(native, source.srv.Get(), menu.Get(), renderW, renderH); }
    void ProcessScene(Target& destination) {
        Bind(destination, scene.srv.Get(), process.Get(), 64, 64);
        context->OMSetBlendState(nullptr, nullptr, UINT(-1)); context->Draw(3, 0);
    }
    void NoDebugErrors() {
        if (!info) return;
        for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T length = 0; info->GetMessage(i, nullptr, &length);
            std::vector<unsigned char> bytes(length); auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            HR(info->GetMessage(i, message, &length));
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                std::printf("D3D11: %s\n", message->pDescription); Check(false, "D3D11 debug error");
            }
        }
    }
};

void BasicCaptureAndComposite(Fixture& f)
{
    Check(f.Begin(), "begin separated stereo menu frame"); f.BindMenu();
    const float nativeColor[4]{.05f, .1f, .15f, 1}; f.context->ClearRenderTargetView(f.native.rtv.Get(), nativeColor);
    const Pipeline before(f.context.Get()); DrawToken draw{f.context.Get()};
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "accepted menu draw replayed");
    Check(draw.calls == 1 && f.renderer.HasLayer(), "capture invokes native exactly once into layer");
    Check(Pipeline(f.context.Get()).bytes == before.bytes, "capture restores entire incoming pipeline");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[6 * 64 + 6], {.05f, .1f, .15f, 1}, "suppressed original leaves native scene untouched");
    Check(!f.renderer.BeginFrame(f.frame, 64, 32, 128, 64, f.Targets()) && f.renderer.HasLayer(), "duplicate BeginFrame preserves pending suppressed UI");
    Check(!f.renderer.BeginFrame(f.frame - 1, 64, 32, 128, 64, f.Targets()) && f.renderer.HasLayer(), "stale BeginFrame preserves pending suppressed UI");
    f.ProcessScene(f.left); f.ProcessScene(f.right);
    const auto processed = Read(f.device.Get(), f.context.Get(), f.left);
    const D3D11_VIEWPORT eye{0, 0, 64, 64, 0, 1};
    Check(f.renderer.CanComposite(f.context.Get(), f.left.rtv.Get(), eye), "output preflight succeeds");
    const Pipeline priorComposite(f.context.Get());
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, eye), "left menu after scene processing");
    Check(Pipeline(f.context.Get()).bytes == priorComposite.bytes, "composite restores incoming pipeline");
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture rejected after first eye seals layer");
    Check(f.renderer.Composite(f.context.Get(), f.right.rtv.Get(), 1, eye), "right menu after scene processing");
    Check(!f.renderer.Composite(f.context.Get(), f.right.rtv.Get(), 1, eye), "duplicate eye blend rejected");
    for (int e = 0; e < 2; ++e) {
        auto result = Read(f.device.Get(), f.context.Get(), e ? f.right : f.left);
        for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x) {
            const Pixel ui = f.sourcePixels[(y / 4) * 32 + (x / 4) + e * 16];
            const auto scene = processed[y * 64 + x];
            PixelIs(result[y * 64 + x], {ui.r * ui.a + scene.r * (1 - ui.a), ui.g * ui.a + scene.g * (1 - ui.a),
                ui.b * ui.a + scene.b * (1 - ui.a), 1}, "sharp text-like source and alpha survive scene processing in correct eye");
        }
    }
    f.renderer.ResetFrame(); Check(!f.renderer.HasLayer() && f.renderer.Frame() == 0, "reset expires previous menu");
    Check(!f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, eye), "no stale menu composite after reset");
}

void FallbackFlipAndScissor(Fixture& f)
{
    Check(f.Begin(), "fallback frame begin"); f.BindMenu();
    f.context->OMSetDepthStencilState(f.alwaysDepth.Get(), 11);
    f.context->RSSetState(f.scissorRaster.Get()); const D3D11_RECT clip{0, 0, 64, 16};
    f.context->RSSetScissorRects(1, &clip);
    DrawToken draw{f.context.Get()}; Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "safe depth ALWAYS and scaled scissor accepted");
    const float background[4]{.125f, .25f, .375f, .2f};
    f.context->ClearRenderTargetView(f.left.rtv.Get(), background);
    f.context->ClearRenderTargetView(f.right.rtv.Get(), background);
    const D3D11_VIEWPORT fallback{8, 10, 32, 32, 0, 1};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, fallback, false), "render-size fallback viewport");
    Check(f.renderer.Composite(f.context.Get(), f.right.rtv.Get(), 1, fallback, true), "flipped fallback viewport");
    for (int e = 0; e < 2; ++e) {
        const auto result = Read(f.device.Get(), f.context.Get(), e ? f.right : f.left);
        for (UINT y = 0; y < 64; ++y) for (UINT x = 0; x < 64; ++x) {
            Pixel expected{.125f, .25f, .375f, .2f};
            if (x >= 8 && x < 40 && y >= 10 && y < 42) {
                const UINT sourceY = e ? (31 - (y - 10)) / 2 : (y - 10) / 2;
                Pixel ui = f.sourcePixels[sourceY * 32 + (x - 8) / 2 + e * 16];
                if (sourceY >= 8) ui.a = 0;
                expected.r = ui.r * ui.a + expected.r * (1 - ui.a);
                expected.g = ui.g * ui.a + expected.g * (1 - ui.a);
                expected.b = ui.b * ui.a + expected.b * (1 - ui.a);
            }
            PixelIs(result[y * 64 + x], expected, "fallback crop, flip, scissor and original target alpha");
        }
    }
}

void OrderAndRollback(Fixture& f)
{
    Check(f.Begin(), "ordered overlays frame"); f.BindMenu();
    DrawToken draw{f.context.Get()}; Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "first straight-alpha overlay");
    f.context->PSSetShader(f.premultiplied.Get(), nullptr, 0); f.context->OMSetBlendState(f.pre.Get(), nullptr, UINT(-1));
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "second premultiplied overlay");
    const float zero[4]{0, 0, 0, 1}; f.context->ClearRenderTargetView(f.left.rtv.Get(), zero);
    const D3D11_VIEWPORT eye{0, 0, 64, 64, 0, 1};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, eye), "ordered alpha layers composite");
    const auto pixels = Read(f.device.Get(), f.context.Get(), f.left);
    PixelIs(pixels[4 * 64 + 20], {.75f, .375f, .1875f, 1}, "two half-alpha draws accumulated in order");

    Check(f.Begin(), "mixed draw rollback frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), zero);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "panel captured before unsupported highlight");
    f.context->OMSetBlendState(f.additive.Get(), nullptr, UINT(-1));
    const Pipeline before(f.context.Get());
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "additive later draw triggers ordered native fallback");
    Check(Pipeline(f.context.Get()).bytes == before.bytes, "rollback preserves later draw pipeline");
    Check(!f.renderer.HasLayer(), "rollback cancels final menu overlay");
    NativeDraw(&draw);
    const auto native = Read(f.device.Get(), f.context.Get(), f.native);
    PixelIs(native[2 * 64 + 10], {1.5f, .75f, .375f, 1}, "restored panel precedes native additive highlight");
    f.context->OMSetBlendState(f.straight.Get(), nullptr, UINT(-1));
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture disabled remainder of aborted frame");

    Check(f.Begin(), "unknown-source rollback frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), zero);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before later unrecognized menu source");
    f.context->PSSetShaderResources(0, 1, f.other.srv.GetAddressOf());
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw) && !f.renderer.HasLayer(), "unrecognized later MENUBG draw restores earlier batch");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "unknown-source rollback preserved captured RGB");

    Check(f.Begin(), "explicit submitted-target restore frame"); f.BindMenu();
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "captured before submit contract change");
    f.context->ClearRenderTargetView(f.other.rtv.Get(), zero);
    Check(f.renderer.RestoreToTarget(f.context.Get(), f.other.rtv.Get()) && !f.renderer.HasLayer(), "restore into actual submitted atlas before fallback filters");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "explicit restore preserves correct stereo coordinates");
}

void RejectionsAndResize(Fixture& f)
{
    DrawToken draw{f.context.Get()};
    auto rejected = [&](const char* message) {
        const Pipeline before(f.context.Get()); const unsigned calls = draw.calls;
        Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), message);
        Check(draw.calls == calls && !f.renderer.HasLayer(), "rejected draw never reissued or suppressed");
        Check(Pipeline(f.context.Get()).bytes == before.bytes, "rejected draw preserves state");
    };
    Check(f.Begin(), "missing source frame"); f.BindMenu();
    ID3D11ShaderResourceView* noSource = nullptr; f.context->PSSetShaderResources(0, 1, &noSource); rejected("missing source rejected");
    f.Bind(f.other, f.source.srv.Get(), f.menu.Get(), 64, 32); rejected("different destination rejected");
    Check(f.Begin(), "depth writing rejection frame"); f.BindMenu(); f.context->OMSetDepthStencilState(f.writeDepth.Get(), 0); rejected("depth-writing draw rejected");
    Check(f.Begin(), "position sampling rejection frame"); f.BindMenu(); f.context->PSSetShader(f.positionBased.Get(), nullptr, 0); rejected("unsafe pixel-coordinate shader rejected");
    Check(f.Begin(), "MRT rejection frame"); f.BindMenu();
    ID3D11RenderTargetView* mrt[2]{f.native.rtv.Get(), f.other.rtv.Get()}; f.context->OMSetRenderTargets(2, mrt, nullptr); rejected("multiple target draw rejected");
    Check(f.Begin(), "viewport rejection frame"); f.BindMenu();
    const D3D11_VIEWPORT outside{0, 0, 65, 32, 0, 1}; f.context->RSSetViewports(1, &outside); rejected("out-of-bounds viewport rejected");
    Check(f.Begin(), "predication rejection frame"); f.BindMenu();
    ComPtr<ID3D11Predicate> predicate; D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    HR(f.device->CreatePredicate(&query, &predicate)); f.context->Begin(predicate.Get()); f.context->End(predicate.Get());
    f.context->SetPredication(predicate.Get(), TRUE); rejected("predicated menu draw rejected"); f.context->SetPredication(nullptr, FALSE);
    Check(f.Begin(), "stream-out rejection frame"); f.BindMenu();
    ComPtr<ID3D11Buffer> stream; D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 256; buffer.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    HR(f.device->CreateBuffer(&buffer, nullptr, &stream)); UINT offset = 32;
    f.context->SOSetTargets(1, stream.GetAddressOf(), &offset); rejected("stream-output draw rejected");
    Check(f.Begin(), "UAV rejection frame"); f.BindMenu();
    ComPtr<ID3D11Buffer> storage; ComPtr<ID3D11UnorderedAccessView> uav;
    buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS; buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; buffer.StructureByteStride = 16;
    HR(f.device->CreateBuffer(&buffer, nullptr, &storage));
    D3D11_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = 16;
    HR(f.device->CreateUnorderedAccessView(storage.Get(), &uv, &uav)); f.context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
    const Pipeline computeBindings(f.context.Get());
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "unrelated compute UAV does not block graphics menu draw");
    Check(Pipeline(f.context.Get()).bytes == computeBindings.bytes, "capture preserves unrelated compute UAV binding");
    Check(f.Begin(), "output UAV rejection frame"); f.BindMenu();
    f.context->OMSetRenderTargetsAndUnorderedAccessViews(1, f.native.rtv.GetAddressOf(), nullptr, 1, 1, uav.GetAddressOf(), nullptr);
    rejected("active output UAV rejected");
    f.context->ClearState();
    auto targets = f.Targets(); targets.version += 1;
    Check(!f.renderer.BeginFrame(++f.frame, 64, 32, 128, 64, targets), "unknown target contract version rejected");
    targets = f.Targets(); targets.sources[0] = nullptr;
    Check(!f.renderer.BeginFrame(++f.frame, 64, 32, 128, 64, targets), "empty source set rejected");
    Check(!f.renderer.BeginFrame(++f.frame, 63, 32, 128, 64, f.Targets()), "mismatched render dimensions rejected");
    Check(!f.renderer.BeginFrame(++f.frame, 64, 32, 127, 64, f.Targets()), "odd stereo width rejected");
    Check(f.Begin(64, 32), "resize from 128x64 to 64x32"); f.BindMenu();
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture after atlas resize");
    const float clear[4]{0, 0, 0, 1}; f.context->ClearRenderTargetView(f.left.rtv.Get(), clear);
    const D3D11_VIEWPORT size{0, 0, 32, 32, 0, 1};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, size), "composite resized atlas");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.left)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "new dimensions use new atlas mapping");
    f.renderer.ResetFrame(true); Check(!f.renderer.HasLayer(), "menu exit releases layer and expires transaction");
    Check(f.Begin(), "atlas reacquired after menu close");
    const D3D11_VIEWPORT invalid{0, 0, std::numeric_limits<float>::infinity(), 32, 0, 1};
    Check(!f.renderer.CanComposite(f.context.Get(), f.left.rtv.Get(), invalid), "nonfinite output viewport rejected before suppression");
    Check(!f.renderer.CanComposite(f.context.Get(), f.left.rtv.Get(), D3D11_VIEWPORT{60, 0, 16, 32, 0, 1}), "oversized output viewport rejected");
    ComPtr<ID3D11DeviceContext> deferred; HR(f.device->CreateDeferredContext(0, &deferred));
    Check(!f.renderer.Initialize(f.device.Get(), deferred.Get()), "deferred context initialization rejected");
    Check(!f.renderer.Capture(deferred.Get(), NativeDraw, &draw), "different/deferred capture context rejected");
}

void CopyLineageAndCrop(Fixture& f)
{
    auto targets = f.Targets(); targets.destinations[1] = f.other.texture.Get();
    Check(f.renderer.BeginFrame(++f.frame, 64, 32, 128, 64, targets), "copy lineage frame");
    f.BindMenu(); DrawToken draw{f.context.Get()}; const float clear[4]{0, 0, 0, 1};
    f.context->ClearRenderTargetView(f.native.rtv.Get(), clear);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before whole MENUBG copy");
    f.renderer.BeforeCopy(f.context.Get(), f.other.texture.Get(), f.native.texture.Get(), true);
    f.context->CopyResource(f.other.texture.Get(), f.native.texture.Get());
    Check(f.renderer.HasLayer(), "compatible copy retains separated layer");
    f.Bind(f.other, f.source.srv.Get(), f.menu.Get(), 64, 32);
    f.context->OMSetBlendState(f.additive.Get(), nullptr, UINT(-1));
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw) && !f.renderer.HasLayer(), "later copied-target draw restores complete lineage");
    NativeDraw(&draw);
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "original copy source restored");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {1.5f, .75f, .375f, 1}, "copied target receives earlier UI before later highlight");

    Check(f.Begin(), "partial-copy rollback frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), clear); f.context->ClearRenderTargetView(f.other.rtv.Get(), clear);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before partial copy");
    f.renderer.BeforeCopy(f.context.Get(), f.other.texture.Get(), f.native.texture.Get(), false);
    Check(!f.renderer.HasLayer(), "partial copy restores source before native operation");
    const D3D11_BOX region{0, 0, 0, 32, 16, 1};
    f.context->CopySubresourceRegion(f.other.texture.Get(), 0, 0, 0, 0, f.native.texture.Get(), 0, &region);
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "partial native copy includes restored UI");

    Check(f.Begin(), "late original draw after copy frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), clear);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture batch before copy snapshot");
    f.renderer.BeforeCopy(f.context.Get(), f.other.texture.Get(), f.native.texture.Get(), true);
    f.context->CopyResource(f.other.texture.Get(), f.native.texture.Get());
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "later original draw becomes native after copy snapshot");
    NativeDraw(&draw);
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.75f, .375f, .1875f, 1}, "late draw only affects original");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "copy snapshot does not receive later draws");

    Check(f.Begin(), "per-eye crop frame"); f.BindMenu();
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before per-eye fallback and cropping");
    f.context->ClearRenderTargetView(f.left.rtv.Get(), clear); f.context->ClearRenderTargetView(f.right.rtv.Get(), clear);
    const D3D11_VIEWPORT vp{4, 2, 32, 32, 0, 1}; const float crop[4]{.25f, 0, .75f, .5f};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, vp, false, crop), "cropped left eye restored independently");
    Check(f.renderer.Composite(f.context.Get(), f.right.rtv.Get(), 1, vp, true, crop), "cropped right eye independently flipped");
    for (int e = 0; e < 2; ++e) {
        const auto result = Read(f.device.Get(), f.context.Get(), e ? f.right : f.left);
        for (UINT y = 0; y < 32; ++y) for (UINT x = 0; x < 32; ++x) {
            const UINT sy = e ? (31 - y) / 4 : y / 4;
            const auto ui = f.sourcePixels[sy * 32 + 4 + x / 4 + e * 16];
            PixelIs(result[(y + 2) * 64 + x + 4], {ui.r * ui.a, ui.g * ui.a, ui.b * ui.a, 1}, "eye crop samples only selected normalized region");
        }
    }
}

void OccupiedPipeline(Fixture& f)
{
    Check(f.Begin(), "occupied compositor state frame"); f.BindMenu(); DrawToken draw{f.context.Get()};
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before unrelated compute/output state");
    const float background[4]{0, 0, 0, 1}; f.context->ClearRenderTargetView(f.left.rtv.Get(), background);
    ComPtr<ID3D11Buffer> constants, vertices, indices, stream, csStorage, omStorage;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 1024; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HR(f.device->CreateBuffer(&bd, nullptr, &constants));
    UINT first = 16, count = 16;
    f.context1->PSSetConstantBuffers1(3, 1, constants.GetAddressOf(), &first, &count);
    f.context1->VSSetConstantBuffers1(5, 1, constants.GetAddressOf(), &first, &count);
    f.context1->CSSetConstantBuffers1(6, 1, constants.GetAddressOf(), &first, &count);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; HR(f.device->CreateBuffer(&bd, nullptr, &vertices));
    UINT stride = 32, vertexOffset = 16; f.context->IASetVertexBuffers(5, 1, vertices.GetAddressOf(), &stride, &vertexOffset);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER; HR(f.device->CreateBuffer(&bd, nullptr, &indices));
    f.context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R32_UINT, 12);
    bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    std::array<float, 256> zeros{}; D3D11_SUBRESOURCE_DATA initial{zeros.data(), 0, 0};
    HR(f.device->CreateBuffer(&bd, &initial, &stream)); UINT streamOffset = 32;
    f.context->SOSetTargets(1, stream.GetAddressOf(), &streamOffset);
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
    HR(f.device->CreateBuffer(&bd, nullptr, &csStorage)); HR(f.device->CreateBuffer(&bd, nullptr, &omStorage));
    ComPtr<ID3D11UnorderedAccessView> csUAV, omUAV;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 64; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    HR(f.device->CreateUnorderedAccessView(csStorage.Get(), &ud, &csUAV)); HR(f.device->CreateUnorderedAccessView(omStorage.Get(), &ud, &omUAV));
    UINT csCount = 17, omCount = 11;
    f.context->CSSetUnorderedAccessViews(0, 1, csUAV.GetAddressOf(), &csCount);
    f.context->OMSetRenderTargetsAndUnorderedAccessViews(1, f.native.rtv.GetAddressOf(), nullptr, 1, 1, omUAV.GetAddressOf(), &omCount);
    f.context->CSSetShaderResources(6, 1, f.other.srv.GetAddressOf()); f.context->CSSetSamplers(4, 1, f.point.GetAddressOf());
    ComPtr<ID3D11Predicate> predicate; const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    HR(f.device->CreatePredicate(&qd, &predicate)); f.context->Begin(predicate.Get()); f.context->End(predicate.Get());
    f.context->SetPredication(predicate.Get(), TRUE);
    const Pipeline before(f.context.Get()); const D3D11_VIEWPORT eye{0, 0, 64, 64, 0, 1};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, eye), "composite isolates predication, SO and UAV state");
    Check(Pipeline(f.context.Get()).bytes == before.bytes, "all captured graphics/compute bindings and constant ranges restored");
    f.context->SetPredication(nullptr, FALSE);
    PixelIs(Read(f.device.Get(), f.context.Get(), f.left)[4 * 64 + 20], {.5f, .25f, .125f, 1}, "incoming false predicate does not hide compositor menu");
    const auto counter = [&](ID3D11UnorderedAccessView* uav) {
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 16; ComPtr<ID3D11Buffer> gpu, cpu;
        HR(f.device->CreateBuffer(&desc, nullptr, &gpu)); f.context->CopyStructureCount(gpu.Get(), 0, uav);
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR(f.device->CreateBuffer(&desc, nullptr, &cpu)); f.context->CopyResource(cpu.Get(), gpu.Get());
        D3D11_MAPPED_SUBRESOURCE map{}; HR(f.context->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &map));
        const UINT value = *static_cast<UINT*>(map.pData); f.context->Unmap(cpu.Get(), 0); return value;
    };
    Check(counter(csUAV.Get()) == 17 && counter(omUAV.Get()) == 11, "UAV append counters preserved");
    const char* soCode = R"HLSL(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
[maxvertexcount(1)] void main(point V input[1], inout PointStream<V> output) { output.Append(input[0]); }
)HLSL";
    auto code = Compile(soCode, "main", "gs_5_0"); ComPtr<ID3D11GeometryShader> gs;
    const D3D11_SO_DECLARATION_ENTRY entry{0, "SV_Position", 0, 0, 4, 0}; const UINT streamStride = 16;
    HR(f.device->CreateGeometryShaderWithStreamOutput(code->GetBufferPointer(), code->GetBufferSize(),
        &entry, 1, &streamStride, 1, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs));
    f.context->OMSetRenderTargets(0, nullptr, nullptr); f.context->GSSetShader(gs.Get(), nullptr, 0);
    f.context->PSSetShader(nullptr, nullptr, 0); f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    f.context->Draw(1, 0); f.context->SOSetTargets(0, nullptr, nullptr);
    D3D11_BUFFER_DESC readDesc{}; stream->GetDesc(&readDesc); readDesc.BindFlags = readDesc.MiscFlags = 0;
    readDesc.Usage = D3D11_USAGE_STAGING; readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging; HR(f.device->CreateBuffer(&readDesc, nullptr, &staging));
    f.context->CopyResource(staging.Get(), stream.Get()); D3D11_MAPPED_SUBRESOURCE mapped{};
    HR(f.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const auto* positions = static_cast<const Pixel*>(mapped.pData);
    PixelIs(positions[0], {0, 0, 0, 0}, "stream-output prefix not overwritten");
    PixelIs(positions[2], {-1, 1, .5f, 1}, "stream-output append offset preserved across composite");
    f.context->Unmap(staging.Get(), 0); f.context->ClearState();
}

void NativeAlphaRollback(Fixture& f)
{
    const float background[4]{0, 0, 0, .2f}; DrawToken draw{f.context.Get()};
    Check(f.Begin(), "native source-over alpha rollback frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), background);
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture native alpha-over draw");
    f.context->OMSetBlendState(f.additive.Get(), nullptr, UINT(-1));
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "unsupported draw restores native alpha-over batch");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, .6f}, "rollback reproduces original alpha write");

    D3D11_BLEND_DESC desc{}; f.straight->GetDesc(&desc);
    desc.RenderTarget[0].RenderTargetWriteMask = 7;
    ComPtr<ID3D11BlendState> preserve; HR(f.device->CreateBlendState(&desc, &preserve));
    Check(f.Begin(), "native alpha-preserving rollback frame"); f.BindMenu();
    f.context->ClearRenderTargetView(f.native.rtv.Get(), background);
    f.context->OMSetBlendState(preserve.Get(), nullptr, UINT(-1));
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "RGB-only native menu draw accepted");
    f.context->OMSetBlendState(f.straight.Get(), nullptr, UINT(-1));
    Check(!f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "alpha-mode change restores previous batch before native draw");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, .2f}, "rollback preserves original alpha for RGB-only draws");
    Check(f.renderer.Stats().restores == 1 && std::strcmp(f.renderer.Stats().lastRejection, "alpha-mode-change") == 0,
        "bounded statistics identify actual fallback reason");
    Check(f.Begin(), "per-eye native alpha repair frame"); f.BindMenu();
    Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before native per-eye input repair");
    f.context->ClearRenderTargetView(f.left.rtv.Get(), background); f.context->ClearRenderTargetView(f.right.rtv.Get(), background);
    const D3D11_VIEWPORT eye{0, 0, 64, 64, 0, 1};
    Check(f.renderer.Composite(f.context.Get(), f.left.rtv.Get(), 0, eye, false, nullptr, true), "per-eye native repair writes original source-over alpha");
    Check(f.renderer.Composite(f.context.Get(), f.right.rtv.Get(), 1, eye), "final eye composition continues preserving destination alpha");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.left)[4 * 64 + 20], {.5f, .25f, .125f, .6f}, "native per-eye input alpha repaired");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.right)[4 * 64 + 20], {.125f, .25f, .5f, .2f}, "final eye alpha remains unchanged");
}

void ClearBarriers(Fixture& f)
{
    DrawToken draw{f.context.Get()}; const float zero[4]{0, 0, 0, 1}, clear[4]{.1f, .2f, .3f, 1};
    const auto beginCopy = [&]() {
        Check(f.Begin(), "clear barrier frame"); f.BindMenu(); f.context->ClearRenderTargetView(f.native.rtv.Get(), zero);
        Check(f.renderer.Capture(f.context.Get(), NativeDraw, &draw), "capture before target overwrite");
        f.renderer.BeforeCopy(f.context.Get(), f.other.texture.Get(), f.native.texture.Get(), true);
        f.context->CopyResource(f.other.texture.Get(), f.native.texture.Get());
    };
    beginCopy();
    f.renderer.BeforeClear(f.context.Get(), f.native.texture.Get()); f.context->ClearRenderTargetView(f.native.rtv.Get(), clear);
    Check(!f.renderer.HasLayer(), "clear of original retires deferred batch");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.1f, .2f, .3f, 1}, "native original clear remains visible");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "earlier copy retains menu after original clear");
    beginCopy();
    f.renderer.BeforeClear(f.context.Get(), f.other.texture.Get()); f.context->ClearRenderTargetView(f.other.rtv.Get(), clear);
    Check(!f.renderer.HasLayer(), "clear of copied target retires deferred batch");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.1f, .2f, .3f, 1}, "native copied-target clear remains visible");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "original retains menu after descendant clear");
    beginCopy();
    f.renderer.BeforeClear(f.context.Get(), f.native.texture.Get()); const D3D11_RECT rect{0, 0, 16, 8};
    f.context1->ClearView(f.native.rtv.Get(), clear, &rect, 1);
    const auto clipped = Read(f.device.Get(), f.context.Get(), f.native);
    PixelIs(clipped[4 * 64 + 10], {.1f, .2f, .3f, 1}, "partial ClearView overwrites only native rectangle");
    PixelIs(clipped[4 * 64 + 20], {1, .5f, .25f, 1}, "partial ClearView preserves restored UI outside rectangle");
    beginCopy();
    f.renderer.BeforeClear(f.context.Get(), f.native.texture.Get()); f.context1->DiscardView(f.native.rtv.Get());
    Check(!f.renderer.HasLayer(), "discard does not leave a stale final menu layer");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "discarded original does not erase previous copy lineage");
    beginCopy(); f.renderer.BeforeClear(f.context.Get(), nullptr);
    Check(!f.renderer.HasLayer(), "unclassified GPU access flushes all pending lineage");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.native)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "null resource barrier restores original");
    PixelIs(Read(f.device.Get(), f.context.Get(), f.other)[2 * 64 + 10], {.5f, .25f, .125f, 1}, "null resource barrier restores copied target");
}
}

int main(int argc, char** argv) try
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    bool debug = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0) debug = true;
        else Check(false, "unknown fixture argument");
    }
    Fixture f(debug);
    BasicCaptureAndComposite(f); std::puts("PASS: stereo text-like detail, processing order, alpha, state and frame expiry");
    FallbackFlipAndScissor(f); std::puts("PASS: fallback viewport, eye selection, vertical flip and scaled scissor");
    OrderAndRollback(f); std::puts("PASS: straight/premultiplied ordering and automatic/explicit native rollback");
    RejectionsAndResize(f); std::puts("PASS: fail-open validation, resizing and resource reacquisition");
    CopyLineageAndCrop(f); std::puts("PASS: whole/partial copy lineage and per-eye cropped fallback");
    OccupiedPipeline(f); std::puts("PASS: occupied pipeline restoration, UAV counters, predication and SO offsets");
    NativeAlphaRollback(f); std::puts("PASS: native target alpha restoration and mixed-alpha fallback");
    ClearBarriers(f); std::puts("PASS: original/copied ClearRTV, partial ClearView and DiscardView barriers");
    f.NoDebugErrors();
    std::printf("ALL MENU LAYER D3D11 TESTS PASS (%u checks; WARP; debug layer=%s)\n", checks, debug ? (f.info ? "on" : "unavailable") : "off");
    return 0;
}
catch (const std::exception& error)
{
    std::printf("FAIL: %s (%u checks)\n", error.what(), checks);
    return 1;
}
