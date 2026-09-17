#include "FoveationBlackoutRenderer.h"

#include <d3dcompiler.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {
constexpr char Shader[] = R"HLSL(
cbuffer BlackoutConstants : register(b0) {
    float2 center; float innerSquared; float middleSquared;
    float inverseWidthSquared; float cutoffSquared; uint regions; uint flipY;
    float2 viewportOrigin; float2 viewportSize;
};
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float2 uv = (position.xy - viewportOrigin) / viewportSize;
    if (flipY != 0) uv.y = 1 - uv.y;
    float2 delta = 2 * (uv - center);
    float radiusSquared = delta.x * delta.x * inverseWidthSquared + delta.y * delta.y;
    bool hidden = ((regions & 1) != 0 && radiusSquared > innerSquared && radiusSquared <= middleSquared)
        || ((regions & 2) != 0 && radiusSquared > middleSquared)
        || ((regions & 4) != 0 && radiusSquared > cutoffSquared);
    if (!hidden) discard;
    return float4(0, 0, 0, 1);
}
)HLSL";

struct Constants {
    float center[2], innerSquared, middleSquared;
    float inverseWidthSquared, cutoffSquared;
    UINT regions, flipY;
    float viewportOrigin[2], viewportSize[2];
};
static_assert(sizeof(Constants) == 48);

bool ValidViewport(const D3D11_VIEWPORT& viewport, UINT width, UINT height)
{
    return std::isfinite(viewport.TopLeftX) && std::isfinite(viewport.TopLeftY) &&
        std::isfinite(viewport.Width) && std::isfinite(viewport.Height) &&
        std::isfinite(viewport.MinDepth) && std::isfinite(viewport.MaxDepth) &&
        viewport.TopLeftX >= 0 && viewport.TopLeftY >= 0 && viewport.Width > 0 &&
        viewport.Height > 0 && double(viewport.TopLeftX) + viewport.Width <= width &&
        double(viewport.TopLeftY) + viewport.Height <= height &&
        viewport.MinDepth >= 0 && viewport.MaxDepth <= 1 && viewport.MinDepth <= viewport.MaxDepth;
}
}

bool FoveationBlackoutRenderer::Initialize(ID3D11Device* device)
{
    if (!device) return false;
    if (device_.Get() == device && state_) return true;
    // Build a complete replacement locally so partial initialization cannot arm
    // culling, or destroy a renderer that still owns a different device's frame.
    FoveationBlackoutRenderer next;
    next.device_ = device;
    device->GetImmediateContext(&next.context_);
    ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
        FAILED(next.context_.As(&next.context1_))) return false;
    auto level = device->GetFeatureLevel();
    if (level < D3D_FEATURE_LEVEL_11_0) return false;
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ?
        D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device), nullptr, &next.state_))) return false;

    ComPtr<ID3DBlob> vs, ps;
    constexpr UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(Shader, sizeof(Shader) - 1, "FoveationBlackout", nullptr, nullptr,
            "VSMain", "vs_5_0", compileFlags, 0, &vs, nullptr)) ||
        FAILED(D3DCompile(Shader, sizeof(Shader) - 1, "FoveationBlackout", nullptr, nullptr,
            "PSMain", "ps_5_0", compileFlags, 0, &ps, nullptr)) ||
        FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &next.vertex_)) ||
        FAILED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &next.pixel_))) return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Constants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &next.constants_))) return false;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster, &next.raster_))) return false;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depth, &next.depth_))) return false;
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &next.blend_))) return false;
    *this = std::move(next);
    return true;
}

bool FoveationBlackoutRenderer::Apply(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
    const ocu_foveation::BlackoutFrame& frame, int eye, const D3D11_VIEWPORT& viewport, bool flipY)
{
    if (!frame.mask.Active() || !frame.frameId) return true;
    if (!frame.Active() || eye < 0 || eye > 1 || !state_ || !target ||
        context != context_.Get() || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> targetDevice;
    target->GetDevice(&targetDevice);
    if (targetDevice.Get() != device_.Get()) return false;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    target->GetDesc(&view);
    if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D) return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    target->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const UINT width = std::max(1u, desc.Width >> view.Texture2D.MipSlice);
    const UINT height = std::max(1u, desc.Height >> view.Texture2D.MipSlice);
    if (!ValidViewport(viewport, width, height)) return false;

    Constants data{};
    std::memcpy(data.center, frame.centers[eye], sizeof(data.center));
    data.innerSquared = frame.inner * frame.inner;
    data.middleSquared = frame.middle * frame.middle;
    data.inverseWidthSquared = 1 / (frame.horizontalScale * frame.horizontalScale);
    const float cutoff = static_cast<float>(ocu_foveation::BlackoutCutoff(frame.mask, frame.middle));
    data.cutoffSquared = cutoff * cutoff;
    data.regions = (frame.mask.middle ? 1u : 0u) | (frame.mask.outer ? 2u : 0u) | (frame.mask.cutoff ? 4u : 0u);
    data.flipY = flipY ? 1u : 0u;
    data.viewportOrigin[0] = viewport.TopLeftX; data.viewportOrigin[1] = viewport.TopLeftY;
    data.viewportSize[0] = viewport.Width; data.viewportSize[1] = viewport.Height;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    std::memcpy(mapped.pData, &data, sizeof(data));
    context->Unmap(constants_.Get(), 0);

    // State objects preserve all graphics/compute bindings, D3D11.1 constant
    // ranges, output UAV counters, stream-output offsets and predication. The
    // existing scene-guard swap hook refreshes its metadata on both swaps.
    ComPtr<ID3DDeviceContextState> previous;
    context1_->SwapDeviceContextState(state_.Get(), &previous);
    context->SetPredication(nullptr, FALSE);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex_.Get(), nullptr, 0);
    context->PSSetShader(pixel_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    context->RSSetState(raster_.Get());
    context->RSSetViewports(1, &viewport);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->OMSetBlendState(blend_.Get(), nullptr, UINT(-1));
    context->OMSetRenderTargets(1, &target, nullptr);
    context->Draw(3, 0);
    // Do not retain an acquired runtime image in our inactive state object.
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context1_->SwapDeviceContextState(previous.Get(), nullptr);
    return true;
}
