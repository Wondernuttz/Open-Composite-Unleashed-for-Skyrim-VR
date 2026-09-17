#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "OpenOVR/Compositor/DensityMaskManager.h"

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char*) {}
void oovr_log_raw_format(const char*, long, const char*, const char*, ...) {}
static unsigned checks = 0;
static void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static void HR(HRESULT result)
{
    if (FAILED(result)) { std::printf("HRESULT=%08X\n", unsigned(result)); throw std::runtime_error("D3D11 operation failed"); }
}
static std::vector<unsigned> Read(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* source, unsigned components)
{
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> copy; HR(device->CreateTexture2D(&desc, nullptr, &copy));
    context->CopyResource(copy.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(copy.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<unsigned> data(size_t(desc.Width) * desc.Height * components);
    for (unsigned y = 0; y < desc.Height; ++y)
        std::memcpy(data.data() + size_t(y) * desc.Width * components,
            static_cast<const char*>(mapped.pData) + size_t(y) * mapped.RowPitch,
            size_t(desc.Width) * components * sizeof(unsigned));
    context->Unmap(copy.Get(), 0); return data;
}

static void Run(D3D_DRIVER_TYPE driver)
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level;
    HR(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
        D3D11_SDK_VERSION, &device, &level, &context));
    ComPtr<ID3D11InfoQueue> debug; HR(device.As(&debug));
    ComPtr<ID3D11DeviceContext1> context1; context.As(&context1);
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    const bool offsets = context1 && SUCCEEDED(device->CheckFeatureSupport(
        D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options))) && options.ConstantBufferOffsetting;

    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 8;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth; HR(device->CreateTexture2D(&td, nullptr, &depth));
    ComPtr<ID3D11DepthStencilView> dsv; HR(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
    td.Format = DXGI_FORMAT_R32G32_UINT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color; HR(device->CreateTexture2D(&td, nullptr, &color));
    ComPtr<ID3D11RenderTargetView> rtv; HR(device->CreateRenderTargetView(color.Get(), nullptr, &rtv));

    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 1024; bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const std::array<unsigned, 256> values{}; D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = values.data();
    ComPtr<ID3D11Buffer> cb; HR(device->CreateBuffer(&bd, &initial, &cb));
    const UINT cbFirst = 16, cbCount = 16;
    constexpr char code[] = R"HLSL(
float4 main(uint vertex:SV_VertexID):SV_POSITION {
    float2 uv=float2((vertex<<1)&2,vertex&2);
    return float4(uv*float2(2,-2)+float2(-1,1),0.5,1);
}
)HLSL";
    ComPtr<ID3DBlob> compiled; HR(D3DCompile(code, sizeof(code)-1, nullptr, nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &compiled, nullptr));
    ComPtr<ID3D11VertexShader> vs; HR(device->CreateVertexShader(compiled->GetBufferPointer(),
        compiled->GetBufferSize(), nullptr, &vs));
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster; HR(device->CreateRasterizerState(&rd, &raster));
    D3D11_BLEND_DESC blendDesc{}; blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    ComPtr<ID3D11BlendState> blend; HR(device->CreateBlendState(&blendDesc, &blend));
    D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL; ds.StencilEnable = TRUE;
    ds.StencilReadMask = ds.StencilWriteMask = 255;
    ds.FrontFace.StencilFunc = ds.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    ds.FrontFace.StencilPassOp = ds.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    ds.FrontFace.StencilFailOp = ds.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    ds.FrontFace.StencilDepthFailOp = ds.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
    ComPtr<ID3D11DepthStencilState> depthState; HR(device->CreateDepthStencilState(&ds, &depthState));
    const float factors[4]{.11f, .22f, .33f, .44f};
    const D3D11_VIEWPORT viewport{0, 0, 8, 8, 0, 1};

    for (unsigned session = 0; session < 2; ++session) {
        DensityMaskManager manager; Check(manager.Initialize(device.Get()), "manager initialization");
        Check(manager.PrepareDepthGuide(depth.Get()), "ownership guide preparation");
        unsigned owner = 0;
        for (unsigned variant = 0; variant < 4; ++variant) {
            // Exercise both the no-blend fast path and a real nondefault state,
            // including a sparse highest RTV slot and optional b0 subranges.
            auto* expectedBlend = (variant & 1) ? blend.Get() : nullptr;
            const UINT expectedMask = (variant & 2) ? 0xaaaaaaaau : 0xffffffffu;
            std::array<ID3D11RenderTargetView*, 8> targets{}; targets[7] = rtv.Get();
            context->OMSetRenderTargets(UINT(targets.size()), targets.data(), dsv.Get());
            context->OMSetBlendState(expectedBlend, factors, expectedMask);
            context->OMSetDepthStencilState(depthState.Get(), 73);
            context->PSSetShader(nullptr, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
            if (offsets) context1->PSSetConstantBuffers1(0, 1, cb.GetAddressOf(), &cbFirst, &cbCount);
            context->VSSetShader(vs.Get(), nullptr, 0); context->RSSetState(raster.Get());
            context->RSSetViewports(1, &viewport); context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
            const FLOAT colorValue[4]{12, 34, 0, 0}; context->ClearRenderTargetView(rtv.Get(), colorValue);
            const auto originalColor = Read(device.Get(), context.Get(), color.Get(), 2);
            auto checkState = [&] {
                ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps, nullptr, nullptr);
                Check(!ps, "original null PS must be restored");
                ComPtr<ID3D11Buffer> actualCB; UINT first = 0, count = 0;
                if (offsets) {
                    context1->PSGetConstantBuffers1(0, 1, &actualCB, &first, &count);
                    Check(first == cbFirst && count == cbCount, "b0 subrange must survive guide replay");
                } else context->PSGetConstantBuffers(0, 1, &actualCB);
                Check(actualCB == cb, "original b0 binding must be restored");
                ID3D11RenderTargetView* actualTargets[8]{}; ComPtr<ID3D11DepthStencilView> actualDSV;
                context->OMGetRenderTargets(8, actualTargets, &actualDSV);
                for (unsigned i = 0; i < 8; ++i) {
                    Check(actualTargets[i] == targets[i], "sparse MRT binding changed");
                    if (actualTargets[i]) actualTargets[i]->Release();
                }
                Check(actualDSV == dsv, "original DSV must be restored");
                ComPtr<ID3D11DepthStencilState> actualDepth; UINT stencil = 0;
                context->OMGetDepthStencilState(&actualDepth, &stencil);
                Check(actualDepth == depthState && stencil == 73, "depth/stencil state or reference changed");
                ComPtr<ID3D11BlendState> actualBlend; FLOAT actualFactors[4]{}; UINT actualMask = 0;
                context->OMGetBlendState(&actualBlend, actualFactors, &actualMask);
                Check(actualBlend.Get() == expectedBlend && actualMask == expectedMask &&
                    !std::memcmp(factors, actualFactors, sizeof(factors)), "blend state, factor or mask changed");
            };
            auto checkOwner = [&](unsigned expected) {
                ComPtr<ID3D11Resource> resource; manager.DepthGuide()->GetResource(&resource);
                ComPtr<ID3D11Texture2D> texture; HR(resource.As(&texture));
                const auto pixels = Read(device.Get(), context.Get(), texture.Get(), 2);
                for (size_t i = 0; i < pixels.size(); i += 2) {
                    Check(pixels[i] == expected, "guide ownership changed");
                    Check(pixels[i+1] == 0x3f000000u, "guide raster depth changed");
                }
            };
            Check(manager.BeginDepthGuide(dsv.Get(), false), "begin same-draw ownership");
            context->Draw(3, 0); manager.EndDepthGuide(); checkState(); checkOwner(++owner);
            const auto preservedDepth = Read(device.Get(), context.Get(), depth.Get(), 1);
            for (auto pixel : preservedDepth) Check((pixel >> 24) == 73, "same-draw ownership must preserve stencil writes");
            for (unsigned replay = 0; replay < 3; ++replay) {
                Check(manager.BeginDepthGuide(dsv.Get(), true), "begin depth-equal invalidation");
                context->Draw(3, 0); manager.EndDepthGuide(); checkState(); checkOwner(0);
                Check(Read(device.Get(), context.Get(), depth.Get(), 1) == preservedDepth,
                    "invalidation must not alter original depth or stencil");
            }
            Check(Read(device.Get(), context.Get(), color.Get(), 2) == originalColor,
                "guide pass must not touch any original color channel");
        }
        manager.Shutdown(); Check(!manager.DepthGuide(), "shutdown must release guide resources");
        context->ClearState();
    }
    context->Flush();
    for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; HR(debug->GetMessage(i, nullptr, &size));
        std::vector<unsigned char> bytes(size); auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        HR(debug->GetMessage(i, message, &size));
        if (message->Severity == D3D11_MESSAGE_SEVERITY_ERROR || message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            std::puts(message->pDescription); throw std::runtime_error("D3D11 debug layer error");
        }
    }
    std::printf("PASS %s: guide pixel/depth/stencil integrity, null and custom blend, factors/sample masks, sparse MRT, b0 ranges, repeated invalidation and session replacement\n",
        driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware");
}
int main(int argc, char** argv)
{
    try { Run(argc > 1 && !std::strcmp(argv[1], "--hardware") ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP); }
    catch (const std::exception& error) { std::printf("FAIL: %s\n", error.what()); return 1; }
    std::printf("PASS: %u checks\n", checks); return 0;
}
