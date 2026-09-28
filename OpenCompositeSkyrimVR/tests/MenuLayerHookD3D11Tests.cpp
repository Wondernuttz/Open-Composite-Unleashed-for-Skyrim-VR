// Exercise the production D3D11 detours, including capture suppression and copy ordering.
#include "OpenOVR/Compositor/MenuLayerRenderer.h"
#include "OpenOVR/Compositor/MenuShaderMetadata.h"
#include "OpenOVR/Compositor/RDMRenderScope.h"
#include "OpenOVR/Compositor/SkyrimMenuTargets.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include <d3dcompiler.h>
#include <dxgi.h>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*, long, const char*, const char* format, ...)
{
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args); std::puts("");
}
namespace {
unsigned checks = 0;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
void HR(HRESULT result) { Check(SUCCEEDED(result), "D3D11 operation failed"); }
struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};
Target Color(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 16; desc.Height = 8;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Target result; HR(device->CreateTexture2D(&desc, nullptr, &result.texture));
    HR(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv));
    HR(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv)); return result;
}
ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    HR(result); return code;
}
struct Hooks {
    static inline Hooks* active = nullptr;
    MenuLayerRenderer& renderer;
    ID3D11DeviceContext* context;
    unsigned draws = 0, captures = 0, nativeCaptures = 0, copies = 0, accesses = 0;
    bool enabled = false;
    struct Native { Hooks* hooks; void (*draw)(void*); void* token; };
    static void NativeDraw(void* token) {
        auto& call = *static_cast<Native*>(token); ++call.hooks->nativeCaptures; call.draw(call.token);
    }
    static bool Draw(ID3D11DeviceContext* context, void (*draw)(void*), void* token) {
        auto& self = *active; ++self.draws; Native native{&self, draw, token};
        const bool captured = self.renderer.Capture(context, NativeDraw, &native);
        if (captured) ++self.captures; return captured;
    }
    static void Copy(ID3D11DeviceContext* context, ID3D11Resource* destination, ID3D11Resource* source, bool whole) {
        ++active->copies; active->renderer.BeforeCopy(context, destination, source, whole);
    }
    static void Access(ID3D11DeviceContext* context, ID3D11Resource* resource) {
        ++active->accesses; active->renderer.BeforeClear(context, resource);
    }
    void Set(bool value) {
        Check(RDMRenderScope::SetDrawInterceptor(value ? context : nullptr, value ? Draw : nullptr,
            value ? Copy : nullptr, value ? Access : nullptr), "draw/copy/access interceptor registration"); enabled = value;
    }
    Hooks(MenuLayerRenderer& value, ID3D11DeviceContext* c) : renderer(value), context(c) { active = this; Set(true); }
    ~Hooks() { RDMRenderScope::SetDrawInterceptor(nullptr, nullptr); active = nullptr; }
};
void Pixels(ID3D11Device* device, Hooks& hooks, const Target& target, const float (&expected)[4], const char* label)
{
    // A staging read is an unsupported game copy and would restore pending UI.
    // Temporarily detach only the callbacks to inspect the pre-copy native pixels.
    const bool enabled = hooks.enabled; if (enabled) hooks.Set(false);
    D3D11_TEXTURE2D_DESC desc{}; target.texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&desc, nullptr, &staging));
    hooks.context->CopyResource(staging.Get(), target.texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(hooks.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    bool match = true;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
        for (UINT x = 0; x < desc.Width; ++x) for (unsigned c = 0; c < 4; ++c)
            match &= std::fabs(row[x * 4 + c] - expected[c]) < .004f;
    }
    hooks.context->Unmap(staging.Get(), 0); if (enabled) hooks.Set(true); Check(match, label);
}
void OneTriangle(ID3D11Device* device, ID3D11DeviceContext* context)
{
    const D3D11_QUERY_DESC desc{D3D11_QUERY_PIPELINE_STATISTICS, 0}; ComPtr<ID3D11Query> query;
    HR(device->CreateQuery(&desc, &query)); context->Begin(query.Get()); context->Draw(3, 0); context->End(query.Get());
    D3D11_QUERY_DATA_PIPELINE_STATISTICS data{}; HRESULT result = S_FALSE;
    for (unsigned i = 0; i < 1000 && result == S_FALSE; ++i) {
        result = context->GetData(query.Get(), &data, sizeof(data), 0); if (result == S_FALSE) Sleep(1);
    }
    Check(result == S_OK && data.IAVertices == 3 && data.IAPrimitives == 1, "exactly one native triangle executed");
}
void Bound(ID3D11DeviceContext* context, ID3D11RenderTargetView* target, ID3D11PixelShader* shader,
    ID3D11BlendState* blend, ID3D11ShaderResourceView* source, ID3D11RenderTargetView* second = nullptr)
{
    ComPtr<ID3D11RenderTargetView> rt0, rt1; ComPtr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* targets[2]{}; context->OMGetRenderTargets(2, targets, &depth);
    rt0.Attach(targets[0]); rt1.Attach(targets[1]);
    ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps, nullptr, nullptr);
    ComPtr<ID3D11ShaderResourceView> srv; context->PSGetShaderResources(0, 1, &srv);
    ComPtr<ID3D11BlendState> state; float factors[4]{}; UINT mask{};
    context->OMGetBlendState(&state, factors, &mask);
    UINT count = 1; D3D11_VIEWPORT view{}; context->RSGetViewports(&count, &view);
    static unsigned boundary = 0; ++boundary;
    const bool preserved = rt0.Get() == target && rt1.Get() == second && !depth && ps.Get() == shader && state.Get() == blend &&
        srv.Get() == source && mask == UINT(-1) && count == 1 && view.Width == 16 && view.Height == 8 &&
        view.TopLeftX == 0 && view.TopLeftY == 0 && view.MinDepth == 0 && view.MaxDepth == 1;
    if (!preserved) std::printf("Binding boundary %u: rt0=%d rt1=%d depth=%p ps=%d blend=%d srv=%d mask=%08X view=%u/%g,%g,%g,%g,%g,%g\n",
        boundary, rt0.Get() == target, rt1.Get() == second, depth.Get(), ps.Get() == shader, state.Get() == blend,
        srv.Get() == source, mask, count, view.TopLeftX, view.TopLeftY, view.Width, view.Height, view.MinDepth, view.MaxDepth);
    Check(preserved, "native target/shader/blend/source/viewport state preserved");
}
constexpr char Shaders[] = R"HLSL(
Texture2D<float4> source : register(t0); SamplerState pointSampler : register(s0);
Texture2D<float4> copied : register(t1);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2,-2) + float2(-1,1), .5, 1); return v;
}
float4 Menu(Vertex v) : SV_Target0 { return source.Sample(pointSampler, v.uv); }
float4 ReadBoundCopy(Vertex v) : SV_Target0 { return copied.Sample(pointSampler, v.uv); }
struct Colors { float4 a : SV_Target0; float4 b : SV_Target1; };
Colors MRT(Vertex v) { Colors c; c.a = float4(.2,.3,.4,1); c.b = float4(.7,.6,.5,1); return c; }
)HLSL";
void Run(bool debug, bool hardware)
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HR(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
        nullptr, debug ? D3D11_CREATE_DEVICE_DEBUG : 0,
        &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC adapterDesc{};
    HR(device.As(&dxgi)); HR(dxgi->GetAdapter(&adapter)); HR(adapter->GetDesc(&adapterDesc));
    std::printf("Adapter: %ls; vendor=0x%04X; driver=%s; device=%s\n", adapterDesc.Description,
        adapterDesc.VendorId, hardware ? "hardware" : "WARP", debug ? "debug" : "nondebug");
    Check(ocu_vrs_guard::InstallShaderCapture(device.Get()), "shader creation hook installed before all shaders");
    auto code = Compile(Shaders, "VS", "vs_5_0"); ComPtr<ID3D11VertexShader> vs;
    HR(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs));
    ComPtr<ID3D11PixelShader> menu, mrt, readCopy; code = Compile(Shaders, "Menu", "ps_5_0");
    HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &menu));
    code = Compile(Shaders, "MRT", "ps_5_0"); HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &mrt));
    code = Compile(Shaders, "ReadBoundCopy", "ps_5_0");
    HR(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &readCopy));
    Check(ocu_menu::ShaderCanScale(menu.Get()), "UV sample shader has production scale metadata");
    D3D11_SAMPLER_DESC sample{}; sample.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sample.AddressU = sample.AddressV = sample.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sample.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler; HR(device->CreateSamplerState(&sample, &sampler));
    D3D11_BLEND_DESC bd{}; auto& b = bd.RenderTarget[0]; b.BlendEnable = TRUE;
    b.SrcBlend = D3D11_BLEND_SRC_ALPHA; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD; b.SrcBlendAlpha = D3D11_BLEND_ONE;
    b.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> straight, additive; HR(device->CreateBlendState(&bd, &straight));
    b.SrcBlend = b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_ONE; b.SrcBlendAlpha = D3D11_BLEND_ZERO;
    HR(device->CreateBlendState(&bd, &additive));
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthFunc = D3D11_COMPARISON_ALWAYS; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D11DepthStencilState> noDepth; HR(device->CreateDepthStencilState(&dd, &noDepth));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; HR(device->CreateRasterizerState(&rd, &raster));
    Target red = Color(device.Get()), green = Color(device.Get()), native = Color(device.Get()), copy = Color(device.Get()), other = Color(device.Get());
    const float redColor[4]{1,0,0,.5f}, greenColor[4]{0,1,0,.25f}, background[4]{.1f,.2f,.3f,1};
    context->ClearRenderTargetView(red.rtv.Get(), redColor); context->ClearRenderTargetView(green.rtv.Get(), greenColor);
    MenuLayerRenderer renderer; Check(renderer.Initialize(device.Get(), context.Get()), "menu renderer initialized");
    Hooks hooks(renderer, context.Get());
    auto bind = [&](Target& target, Target& source, ID3D11PixelShader* shader, ID3D11BlendState* blend) {
        context->ClearState(); context->OMSetRenderTargets(1, target.rtv.GetAddressOf(), nullptr);
        context->OMSetBlendState(blend, nullptr, UINT(-1)); context->OMSetDepthStencilState(noDepth.Get(), 0);
        context->RSSetState(raster.Get()); const D3D11_VIEWPORT vp{0,0,16,8,0,1}; context->RSSetViewports(1, &vp);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(shader, nullptr, 0); context->PSSetShaderResources(0, 1, source.srv.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    };
    // Registered hooks with no active menu transaction leave MRT and copy work native.
    bind(native, red, mrt.Get(), nullptr); ID3D11RenderTargetView* targets[]{native.rtv.Get(), other.rtv.Get()};
    context->OMSetRenderTargets(2, targets, nullptr); OneTriangle(device.Get(), context.Get());
    Check(hooks.draws == 1 && hooks.captures == 0 && hooks.nativeCaptures == 0, "inactive draw bypasses capture");
    context->CopyResource(copy.texture.Get(), native.texture.Get()); Check(hooks.copies >= 1, "real copy hook invoked");
    Bound(context.Get(), native.rtv.Get(), mrt.Get(), nullptr, red.srv.Get(), other.rtv.Get());
    Pixels(device.Get(), hooks, native, {.2f,.3f,.4f,1}, "inactive MRT target zero");
    Pixels(device.Get(), hooks, other, {.7f,.6f,.5f,1}, "inactive MRT target one");
    Pixels(device.Get(), hooks, copy, {.2f,.3f,.4f,1}, "inactive CopyResource unchanged");
    context->ClearRenderTargetView(native.rtv.Get(), background); context->ClearRenderTargetView(copy.rtv.Get(), background);
    ocu_menu::Targets known{}; known.context = context.Get(); known.sources[0] = red.texture.Get(); known.sources[1] = green.texture.Get();
    known.destinations[0] = native.texture.Get(); known.destinations[1] = copy.texture.Get();
    Check(renderer.BeginFrame(1,16,8,32,16,known), "reserved menu layer before capture");
    bind(native, red, menu.Get(), straight.Get()); const unsigned priorDraws = hooks.draws;
    OneTriangle(device.Get(), context.Get());
    Check(hooks.draws == priorDraws + 1 && hooks.captures == 1 && hooks.nativeCaptures == 1 && renderer.HasLayer(),
        "actual hooked menu draw calls native once and consumes original");
    Bound(context.Get(), native.rtv.Get(), menu.Get(), straight.Get(), red.srv.Get());
    Pixels(device.Get(), hooks, native, background, "captured menu absent from native target");
    const unsigned priorCopies = hooks.copies; context->CopyResource(copy.texture.Get(), native.texture.Get());
    Check(hooks.copies > priorCopies && renderer.HasLayer(), "whole native copy retains pending menu lineage");
    Pixels(device.Get(), hooks, copy, background, "copy contains menu-free scene before fallback");
    bind(copy, green, menu.Get(), additive.Get()); context->Draw(3,0);
    Check(!renderer.HasLayer() && hooks.captures == 1 && hooks.nativeCaptures == 1, "unsupported draw retires capture before native fallback");
    Bound(context.Get(), copy.rtv.Get(), menu.Get(), additive.Get(), green.srv.Get());
    Pixels(device.Get(), hooks, native, {.55f,.1f,.15f,1}, "original receives only captured A");
    Pixels(device.Get(), hooks, copy, {.55f,1.1f,.15f,1}, "copied target receives A before additive B exactly once");
    ComPtr<ID3D11DeviceContext1> context1; HR(context.As(&context1));
    auto capturedCopy = [&](std::uint64_t frame, bool extended = false) {
        context->ClearRenderTargetView(native.rtv.Get(), background);
        context->ClearRenderTargetView(copy.rtv.Get(), background);
        Check(renderer.BeginFrame(frame,16,8,32,16,known), "new menu access test frame reserved");
        bind(native, red, menu.Get(), straight.Get()); context->Draw(3,0);
        Check(renderer.HasLayer(), "new access test captures menu");
        if (extended) context1->CopySubresourceRegion1(copy.texture.Get(), 0, 0, 0, 0, native.texture.Get(), 0, nullptr, 0);
        else context->CopyResource(copy.texture.Get(), native.texture.Get());
        Check(renderer.HasLayer(), "whole native copy keeps deferred menu");
    };
    // A later clear owns its destination, while the earlier copy still owns A.
    capturedCopy(2); unsigned priorAccesses = hooks.accesses;
    context->ClearRenderTargetView(copy.rtv.Get(), background);
    Check(hooks.accesses > priorAccesses && !renderer.HasLayer(), "ClearRenderTargetView restores and retires pending lineage");
    Pixels(device.Get(), hooks, native, {.55f,.1f,.15f,1}, "clear preserves A on original target");
    Pixels(device.Get(), hooks, copy, background, "clear overwrites copied A without later resurrection");
    // Context1 whole copies use the same lineage contract; uploads overwrite it.
    capturedCopy(3, true); Pixels(device.Get(), hooks, copy, background, "Context1 whole copy remains menu-free");
    std::array<std::array<float,4>,16*8> uploaded{};
    for (auto& pixel : uploaded) pixel = {.2f,.4f,.6f,1};
    priorAccesses = hooks.accesses;
    context->UpdateSubresource(copy.texture.Get(), 0, nullptr, uploaded.data(), 16 * sizeof(uploaded[0]), 0);
    Check(hooks.accesses > priorAccesses && !renderer.HasLayer(), "UpdateSubresource restores before native overwrite");
    Pixels(device.Get(), hooks, native, {.55f,.1f,.15f,1}, "upload preserves earlier original A");
    Pixels(device.Get(), hooks, copy, {.2f,.4f,.6f,1}, "upload owns all overwritten copied pixels");
    // Shader consumption must observe complete pixels before its actual draw.
    capturedCopy(4); priorAccesses = hooks.accesses;
    bind(other, copy, menu.Get(), nullptr);
    Check(hooks.accesses > priorAccesses && !renderer.HasLayer(), "SRV binding restores pending source before shader consumption");
    OneTriangle(device.Get(), context.Get());
    Bound(context.Get(), other.rtv.Get(), menu.Get(), nullptr, copy.srv.Get());
    Pixels(device.Get(), hooks, other, {.55f,.1f,.15f,1}, "native sampling sees restored A exactly once");
    // An explicitly boxed Context1 copy cannot propagate a whole-layer promise.
    capturedCopy(5); const D3D11_BOX box{0,0,0,16,8,1}; const unsigned beforeRegionCopies = hooks.copies;
    context1->CopySubresourceRegion1(other.texture.Get(), 0, 0, 0, 0, native.texture.Get(), 0, &box, 0);
    Check(hooks.copies > beforeRegionCopies && !renderer.HasLayer(), "Context1 region copy restores its tracked source");
    Pixels(device.Get(), hooks, other, {.55f,.1f,.15f,1}, "boxed Context1 copy consumes restored pixels");
    // Copy destinations can already be shader inputs; no later SRV setter is required.
    context->ClearRenderTargetView(native.rtv.Get(), background);
    context->ClearRenderTargetView(copy.rtv.Get(), background);
    Check(renderer.BeginFrame(6,16,8,32,16,known), "already-bound copied SRV frame reserved");
    bind(native, red, menu.Get(), straight.Get());
    context->PSSetShaderResources(1, 1, copy.srv.GetAddressOf());
    context->Draw(3,0); Check(renderer.HasLayer(), "unused t1 binding does not block original t0 menu capture");
    context->CopyResource(copy.texture.Get(), native.texture.Get());
    // Only shader/output state changes here; t1 retains its pre-copy binding.
    context->OMSetRenderTargets(1, other.rtv.GetAddressOf(), nullptr);
    context->OMSetBlendState(nullptr, nullptr, UINT(-1)); context->PSSetShader(readCopy.Get(), nullptr, 0);
    ComPtr<ID3D11ShaderResourceView> retained; context->PSGetShaderResources(1, 1, &retained);
    Check(retained.Get() == copy.srv.Get(), "copied destination SRV remained bound across native copy");
    context->Draw(3,0);
    Check(!renderer.HasLayer(), "already-bound copied SRV consumption retires pending menu");
    Pixels(device.Get(), hooks, other, {.55f,.1f,.15f,1}, "already-bound t1 consumer receives restored menu exactly once");
    // Playback does not replay immediate-context setters, so flush before execution.
    capturedCopy(7); ComPtr<ID3D11DeviceContext> deferred; HR(device->CreateDeferredContext(0, &deferred));
    deferred->ClearRenderTargetView(copy.rtv.Get(), background);
    ComPtr<ID3D11CommandList> commands; HR(deferred->FinishCommandList(FALSE, &commands));
    Check(renderer.HasLayer(), "recording deferred clear leaves immediate menu pending");
    priorAccesses = hooks.accesses; context->ExecuteCommandList(commands.Get(), TRUE);
    Check(hooks.accesses > priorAccesses && !renderer.HasLayer(), "command-list execution restores pending menu first");
    Bound(context.Get(), native.rtv.Get(), menu.Get(), straight.Get(), red.srv.Get());
    Pixels(device.Get(), hooks, native, {.55f,.1f,.15f,1}, "deferred descendant clear preserves original A");
    Pixels(device.Get(), hooks, copy, background, "deferred clear remains authoritative after menu restoration");
    hooks.Set(false); const unsigned removedDraws = hooks.draws, removedCopies = hooks.copies, removedAccesses = hooks.accesses;
    bind(native, red, menu.Get(), straight.Get()); OneTriangle(device.Get(), context.Get());
    context->CopyResource(other.texture.Get(), native.texture.Get());
    context->ExecuteCommandList(commands.Get(), TRUE);
    Check(hooks.draws == removedDraws && hooks.copies == removedCopies && hooks.accesses == removedAccesses,
        "removed callbacks never receive subsequent native work");
    Pixels(device.Get(), hooks, other, {.775f,.05f,.075f,1}, "next native draw and copy execute once after callback removal");
    context->ClearState();
}
}
int main(int argc, char** argv)
{
    bool debug = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--debug") == 0) debug = true;
        else if (std::strcmp(argv[i], "--hardware") == 0) hardware = true;
        else { std::fprintf(stderr, "Usage: OCUMenuLayerHookD3D11Test [--debug] [--hardware]\n"); return 2; }
    }
    try { Run(debug, hardware); std::printf("Menu layer real-hook D3D11 tests passed (%u checks, %s %s device)\n",
        checks, hardware ? "hardware" : "WARP", debug ? "debug" : "nondebug"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "Menu layer hook test failed: %s\n", error.what()); return 1; }
}
