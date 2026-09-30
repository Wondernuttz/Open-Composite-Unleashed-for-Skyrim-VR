#include "MenuLayerRenderer.h"
#include "SkyrimMenuTargets.h"
#include "MenuShaderMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace {
constexpr char CompositeShader[] = R"HLSL(
Texture2D<float4> menuLayer : register(t0);
SamplerState menuSampler : register(s0);
cbuffer MenuConstants : register(b0) {
    float2 targetOrigin; float2 targetSize;
    float2 layerSize; uint eye; uint flipY;
    float2 uvOrigin; float2 uvExtent;
};
float4 VSMain(uint index : SV_VertexID) : SV_Position {
    float2 corner = float2((index << 1) & 2, index & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PSMain(float4 position : SV_Position) : SV_Target {
    float2 eyeSize = float2(layerSize.x * (eye < 2 ? 0.5 : 1), layerSize.y);
    float2 uv = (position.xy - targetOrigin) / targetSize;
    if (flipY != 0) uv.y = 1 - uv.y;
    uv = uvOrigin + uv * uvExtent;
    // Clamp within this eye's texel centers: linear filtering must never mix
    // the adjacent eye, including when a warmup frame uses a smaller viewport.
    float2 texel = clamp(uv * eyeSize, float2(0.5, 0.5), eyeSize - 0.5);
    if (eye < 2) texel.x += float(eye) * eyeSize.x;
    return menuLayer.SampleLevel(menuSampler, texel / layerSize, 0);
}
)HLSL";

struct CompositeConstants {
    float origin[2], size[2], layerSize[2];
    UINT eye, flipY;
    float uvOrigin[2], uvExtent[2];
};
static_assert(sizeof(CompositeConstants) == 48);

bool ValidViewport(const D3D11_VIEWPORT& v, UINT width, UINT height)
{
    return std::isfinite(v.TopLeftX) && std::isfinite(v.TopLeftY) &&
        std::isfinite(v.Width) && std::isfinite(v.Height) &&
        std::isfinite(v.MinDepth) && std::isfinite(v.MaxDepth) &&
        v.TopLeftX >= 0 && v.TopLeftY >= 0 && v.Width > 0 && v.Height > 0 &&
        double(v.TopLeftX) + v.Width <= width && double(v.TopLeftY) + v.Height <= height &&
        v.MinDepth >= 0 && v.MaxDepth <= 1 && v.MinDepth <= v.MaxDepth;
}

bool SimpleTexture(ID3D11Texture2D* texture, ID3D11Device* device, D3D11_TEXTURE2D_DESC& desc)
{
    if (!texture) return false;
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    texture->GetDesc(&desc);
    return owner.Get() == device && desc.Width && desc.Height &&
        desc.SampleDesc.Count == 1 && desc.ArraySize == 1;
}

// Capture changes only these states; all other bindings remain available to the
// original VS/PS. Restoration also runs if a caller's draw thunk throws.
struct CaptureState {
    ID3D11DeviceContext* context;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11DepthStencilState> depth;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0, stencilRef = 0;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    bool changed = false;

    explicit CaptureState(ID3D11DeviceContext* value) : context(value)
    {
        ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &dsv);
        for (unsigned i = 0; i < targets.size(); ++i) targets[i].Attach(views[i]);
        context->OMGetBlendState(&blend, blendFactor, &sampleMask);
        context->OMGetDepthStencilState(&depth, &stencilRef);
        context->RSGetViewports(&viewportCount, viewports);
        context->RSGetScissorRects(&scissorCount, scissors);
    }
    ~CaptureState()
    {
        if (!changed) return;
        ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        for (unsigned i = 0; i < targets.size(); ++i) views[i] = targets[i].Get();
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, dsv.Get());
        context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        context->OMSetDepthStencilState(depth.Get(), stencilRef);
        context->RSSetViewports(viewportCount, viewports);
        context->RSSetScissorRects(scissorCount, scissors);
    }
};

bool HasSideEffects(ID3D11DeviceContext* context, UINT uavSlots)
{
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;
    context->GetPredication(&predicate, &predicateValue);
    if (predicate) return true;
    ID3D11Buffer* streamOut[D3D11_SO_BUFFER_SLOT_COUNT]{};
    context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, streamOut);
    bool active = false;
    for (auto* buffer : streamOut) { if (buffer) { active = true; buffer->Release(); } }
    ID3D11UnorderedAccessView* views[D3D11_1_UAV_SLOT_COUNT]{};
    context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavSlots, views);
    for (UINT i = 0; i < uavSlots; ++i) { if (views[i]) { active = true; views[i]->Release(); views[i] = nullptr; } }
    // Draw does not execute the compute stage. Its unrelated UAV bindings are
    // preserved, and cannot alias our newly created private layer texture.
    return active;
}

bool BoundForShaderRead(ID3D11DeviceContext* context, ID3D11Resource* resource)
{
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]{};
    const auto contains = [&]() {
        bool found = false;
        for (auto*& view : views) {
            if (!view) continue;
            ComPtr<ID3D11Resource> bound;
            view->GetResource(&bound);
            found |= bound.Get() == resource;
            view->Release(); view = nullptr;
        }
        return found;
    };
    context->VSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); if (contains()) return true;
    context->HSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); if (contains()) return true;
    context->DSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); if (contains()) return true;
    context->GSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); if (contains()) return true;
    context->PSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); if (contains()) return true;
    context->CSGetShaderResources(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, views); return contains();
}

struct BusyScope {
    bool& flag;
    explicit BusyScope(bool& value) : flag(value) { flag = true; }
    ~BusyScope() { flag = false; }
};
}

bool MenuLayerRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!device || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    ComPtr<ID3D11Device> owner;
    context->GetDevice(&owner);
    if (owner.Get() != device) return false;
    if (device_.Get() == device && context_.Get() == context && compositionContext_) return true;
    MenuLayerRenderer next;
    next.device_ = device;
    next.context_ = context;
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ||
        FAILED(device->CreateDeferredContext(0, &next.compositionContext_))) return false;

    ComPtr<ID3DBlob> vertex, pixel;
    const UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    if (FAILED(D3DCompile(CompositeShader, sizeof(CompositeShader) - 1, "OCU menu layer", nullptr, nullptr,
            "VSMain", "vs_5_0", compileFlags, 0, &vertex, nullptr)) ||
        FAILED(D3DCompile(CompositeShader, sizeof(CompositeShader) - 1, "OCU menu layer", nullptr, nullptr,
            "PSMain", "ps_5_0", compileFlags, 0, &pixel, nullptr)) ||
        FAILED(device->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr, &next.vertex_)) ||
        FAILED(device->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr, &next.pixel_))) return false;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(CompositeConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &next.constants_))) return false;
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler, &next.sampler_))) return false;
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
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend, &next.straightCaptureBlend_))) return false;
    target.SrcBlend = D3D11_BLEND_ONE;
    if (FAILED(device->CreateBlendState(&blend, &next.premultipliedBlend_))) return false;
    // Presentation alpha belongs to the existing eye image. Coverage is used
    // only to blend menu RGB, never to turn an opaque OpenXR eye transparent.
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
        D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(device->CreateBlendState(&blend, &next.compositeBlend_))) return false;
    *this = std::move(next);
    return true;
}

bool MenuLayerRenderer::ReserveLayer(UINT width, UINT height)
{
    if (layer_ && width == layerWidth_ && height == layerHeight_) return true;
    // Release the previous dimensions before allocating the new large atlas.
    layerSRV_.Reset(); layerRTV_.Reset(); layer_.Reset();
    layerWidth_ = layerHeight_ = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture)) ||
        FAILED(device_->CreateRenderTargetView(texture.Get(), nullptr, &rtv)) ||
        FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &srv))) return false;
    layer_ = std::move(texture); layerRTV_ = std::move(rtv); layerSRV_ = std::move(srv);
    layerWidth_ = width; layerHeight_ = height;
    return true;
}

void MenuLayerRenderer::ResetFrame(bool releaseLayer)
{
    if (busy_) return;
    frame_ = 0; renderWidth_ = renderHeight_ = 0;
    captured_ = sealed_ = false; compositedEyes_ = 0;
    nativeAlphaOver_ = false;
    statistics_ = {};
    originalTarget_.Reset();
    for (auto& target : copiedTargets_) target.Reset();
    for (auto& source : sources_) source.Reset();
    for (auto& destination : destinations_) destination.Reset();
    if (releaseLayer) {
        layerSRV_.Reset(); layerRTV_.Reset(); layer_.Reset();
        layerWidth_ = layerHeight_ = 0;
    }
}

bool MenuLayerRenderer::BeginFrame(std::uint64_t frame, UINT renderWidth, UINT renderHeight,
    UINT outputWidth, UINT outputHeight, const ocu_menu::Targets& targets)
{
    if (busy_ || !frame || frame <= lastBegunFrame_) return false;
    ResetFrame();
    constexpr UINT limit = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (!compositionContext_ ||
        !renderWidth || !renderHeight || !outputWidth || !outputHeight || (outputWidth & 1) ||
        renderWidth > limit || renderHeight > limit || outputWidth > limit || outputHeight > limit ||
        targets.size != sizeof(ocu_menu::Targets) || targets.version != ocu_menu::Version ||
        targets.context != context_.Get()) return false;
    bool haveSource = false, haveDestination = false;
    for (unsigned i = 0; i < 4; ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        if (targets.sources[i]) {
            if (!SimpleTexture(targets.sources[i], device_.Get(), desc) ||
                !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return false;
            haveSource = true;
        }
        if (targets.destinations[i]) {
            if (!SimpleTexture(targets.destinations[i], device_.Get(), desc) ||
                !(desc.BindFlags & D3D11_BIND_RENDER_TARGET) ||
                desc.Width != renderWidth || desc.Height != renderHeight) return false;
            haveDestination = true;
        }
    }
    if (!haveSource || !haveDestination || !ReserveLayer(outputWidth, outputHeight)) return false;
    for (unsigned i = 0; i < 4; ++i) {
        sources_[i] = targets.sources[i]; destinations_[i] = targets.destinations[i];
    }
    ComPtr<ID3D11Predicate> predicate;
    BOOL predicateValue = FALSE;
    context_->GetPredication(&predicate, &predicateValue);
    context_->SetPredication(nullptr, FALSE);
    const float transparent[4]{};
    context_->ClearRenderTargetView(layerRTV_.Get(), transparent);
    context_->SetPredication(predicate.Get(), predicateValue);
    renderWidth_ = renderWidth; renderHeight_ = renderHeight;
    frame_ = lastBegunFrame_ = frame;
    return true;
}

bool MenuLayerRenderer::Capture(ID3D11DeviceContext* context, void (*nativeDraw)(void*), void* drawToken)
{
    if (!frame_ || sealed_ || busy_ || !nativeDraw || context != context_.Get()) return false;
    ++statistics_.draws;
    BusyScope busy(busy_);
    // Check resource identity before the expensive pipeline validation. Ordinary
    // world draws almost always fail this first test.
    ComPtr<ID3D11ShaderResourceView> sourceView;
    context->PSGetShaderResources(0, 1, &sourceView);
    ComPtr<ID3D11Resource> source;
    if (sourceView) sourceView->GetResource(&source);
    bool recognized = false;
    for (auto& candidate : sources_) recognized |= candidate && source.Get() == candidate.Get();
    const bool knownSource = recognized;
    if (knownSource) ++statistics_.candidates;
    if (!knownSource && !captured_) return false;
    CaptureState saved(context);
    if (!saved.targets[0]) return false;
    ComPtr<ID3D11Resource> destination;
    saved.targets[0]->GetResource(&destination);
    recognized = false;
    for (auto& candidate : destinations_) recognized |= candidate && destination.Get() == candidate.Get();
    const auto reject = [&](const char* reason = "unsupported-state") {
        statistics_.lastRejection = reason;
        // Deferred and native UI must retain their original order. Restore the
        // prior batch before letting an unsupported later menu draw execute.
        if (captured_) RestoreAllCaptured(context);
        sealed_ = true;
        return false;
    };
    if (!recognized) {
        for (const auto& copy : copiedTargets_) {
            if (!copy) continue;
            ComPtr<ID3D11Resource> copied;
            copy->GetResource(&copied);
            if (copied.Get() == destination.Get()) return reject();
        }
        return false;
    }
    if (originalTarget_) {
        ComPtr<ID3D11Resource> original;
        originalTarget_->GetResource(&original);
        if (original.Get() != destination.Get()) {
            for (const auto& copy : copiedTargets_) {
                if (!copy) continue;
                ComPtr<ID3D11Resource> copied;
                copy->GetResource(&copied);
                if (copied.Get() == destination.Get()) return reject("draw-after-copy");
            }
            // A copy may already have propagated the old scene to this target.
            // Without copy lineage, restoring only the old target would erase
            // the deferred menu from the submitted image. Retain the layer for
            // presentation, and accept no more captures in this transaction.
            sealed_ = true;
            statistics_.lastRejection = "untracked-destination-change";
            return false;
        }
    }
    // Once a copy has propagated this exact batch, further drawing must become
    // native. Adding more deferred UI would incorrectly add those later pixels
    // to copies which predate them when the transaction is restored.
    for (const auto& copy : copiedTargets_) if (copy) return reject("draw-after-copy");
    if (!knownSource || destination.Get() == source.Get()) return reject("source");
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    sourceView->GetDesc(&srv);
    if (srv.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || srv.Texture2D.MostDetailedMip != 0) return reject("source-view");
    for (unsigned i = 1; i < saved.targets.size(); ++i) if (saved.targets[i]) return reject("multiple-targets");
    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
    saved.targets[0]->GetDesc(&rtv);
    if (rtv.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rtv.Texture2D.MipSlice != 0) return reject("target-view");
    if (!CanComposite(context, saved.targets[0].Get(),
            D3D11_VIEWPORT{0, 0, float(renderWidth_), float(renderHeight_), 0, 1})) return reject("target-preflight");
    if (!saved.blend || saved.sampleMask != UINT(-1)) return reject("blend-or-sample-mask");
    D3D11_BLEND_DESC blend{};
    saved.blend->GetDesc(&blend);
    const auto& colorBlend = blend.RenderTarget[0];
    if (blend.AlphaToCoverageEnable || !colorBlend.BlendEnable ||
        colorBlend.BlendOp != D3D11_BLEND_OP_ADD || colorBlend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA ||
        (colorBlend.SrcBlend != D3D11_BLEND_SRC_ALPHA && colorBlend.SrcBlend != D3D11_BLEND_ONE) ||
        (colorBlend.RenderTargetWriteMask & 7) != 7) return reject("blend");
    const bool writesAlpha = (colorBlend.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALPHA) != 0;
    const bool alphaOver = writesAlpha && colorBlend.BlendOpAlpha == D3D11_BLEND_OP_ADD &&
        colorBlend.SrcBlendAlpha == D3D11_BLEND_ONE && colorBlend.DestBlendAlpha == D3D11_BLEND_INV_SRC_ALPHA;
    const bool preservesAlpha = !writesAlpha || (colorBlend.BlendOpAlpha == D3D11_BLEND_OP_ADD &&
        colorBlend.SrcBlendAlpha == D3D11_BLEND_ZERO && colorBlend.DestBlendAlpha == D3D11_BLEND_ONE);
    if (!alphaOver && !preservesAlpha) return reject("alpha-blend");
    if (captured_ && alphaOver != nativeAlphaOver_) return reject("alpha-mode-change");
    D3D11_DEPTH_STENCIL_DESC depth{};
    if (saved.depth) saved.depth->GetDesc(&depth);
    else { depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; }
    // A depth-tested or stencil-tested UI fragment cannot be replayed without
    // its matching depth surface. Leave it on the native path.
    if (depth.StencilEnable || (depth.DepthEnable &&
        (depth.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO || depth.DepthFunc != D3D11_COMPARISON_ALWAYS))) return reject("depth-or-stencil");
    ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs;
    ComPtr<ID3D11DomainShader> ds;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    context->GSGetShader(&gs, nullptr, nullptr); context->HSGetShader(&hs, nullptr, nullptr);
    context->DSGetShader(&ds, nullptr, nullptr); context->VSGetShader(&vs, nullptr, nullptr);
    context->PSGetShader(&ps, nullptr, nullptr);
    if (gs || hs || ds) return reject("geometry-or-tessellation-stage");
    if (!vs || !ps || !ocu_menu::ShaderCanScale(ps.Get())) return reject("shader");
    if (HasSideEffects(context, device_->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ?
            D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT)) return reject("predication-output-uav-or-stream-output");
    if (!saved.viewportCount) return reject("viewport");
    auto viewports = std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>{};
    auto scissors = std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>{};
    const double scaleX = double(layerWidth_) / renderWidth_, scaleY = double(layerHeight_) / renderHeight_;
    for (UINT i = 0; i < saved.viewportCount; ++i) {
        if (!ValidViewport(saved.viewports[i], renderWidth_, renderHeight_)) return reject("viewport");
        viewports[i] = saved.viewports[i];
        viewports[i].TopLeftX = float(saved.viewports[i].TopLeftX * scaleX);
        viewports[i].TopLeftY = float(saved.viewports[i].TopLeftY * scaleY);
        viewports[i].Width = float(saved.viewports[i].Width * scaleX);
        viewports[i].Height = float(saved.viewports[i].Height * scaleY);
    }
    ComPtr<ID3D11RasterizerState> raster;
    context->RSGetState(&raster);
    D3D11_RASTERIZER_DESC rasterDesc{};
    if (raster) raster->GetDesc(&rasterDesc);
    if (rasterDesc.ScissorEnable && !saved.scissorCount) return reject("scissor");
    for (UINT i = 0; i < saved.scissorCount; ++i) {
        const auto& rect = saved.scissors[i];
        if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
            rect.right > LONG(renderWidth_) || rect.bottom > LONG(renderHeight_)) return reject("scissor");
        scissors[i] = { LONG(std::lround(rect.left * scaleX)), LONG(std::lround(rect.top * scaleY)),
            LONG(std::lround(rect.right * scaleX)), LONG(std::lround(rect.bottom * scaleY)) };
    }
    if (!originalTarget_) originalTarget_ = saved.targets[0];
    nativeAlphaOver_ = alphaOver;
    saved.changed = true;
    auto* layerTarget = layerRTV_.Get();
    context->OMSetRenderTargets(1, &layerTarget, nullptr);
    context->OMSetBlendState(colorBlend.SrcBlend == D3D11_BLEND_ONE ?
        premultipliedBlend_.Get() : straightCaptureBlend_.Get(), saved.blendFactor, saved.sampleMask);
    context->OMSetDepthStencilState(depth_.Get(), 0);
    context->RSSetViewports(saved.viewportCount, viewports.data());
    context->RSSetScissorRects(saved.scissorCount, scissors.data());
    nativeDraw(drawToken);
    captured_ = true;
    ++statistics_.captures;
    return true;
}

bool MenuLayerRenderer::Composite(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
    int eye, const D3D11_VIEWPORT& viewport, bool flipY, const float* eyeUVBounds, bool restoreNativeAlpha)
{
    if (!HasLayer() || busy_ || context != context_.Get() || !target || eye < 0 || eye > 1 ||
        (compositedEyes_ & (1u << eye))) return false;
    BusyScope busy(busy_);
    if (!DrawLayer(context, target, eye, viewport, flipY, eyeUVBounds, restoreNativeAlpha && nativeAlphaOver_)) return false;
    sealed_ = true;
    compositedEyes_ |= 1u << eye;
    return true;
}

bool MenuLayerRenderer::RestoreToTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
{
    if (busy_ || context != context_.Get()) return false;
    BusyScope busy(busy_);
    return RestoreCaptured(context, target);
}

bool MenuLayerRenderer::RestoreAllCaptured(ID3D11DeviceContext* context)
{
    if (!HasLayer() || !originalTarget_ || compositedEyes_) return false;
    const D3D11_VIEWPORT viewport{0, 0, float(renderWidth_), float(renderHeight_), 0, 1};
    if (!DrawLayer(context, originalTarget_.Get(), 2, viewport, false, nullptr, nativeAlphaOver_)) return false;
    for (const auto& target : copiedTargets_)
        if (target && !DrawLayer(context, target.Get(), 2, viewport, false, nullptr, nativeAlphaOver_)) return false;
    captured_ = false; sealed_ = true;
    ++statistics_.restores;
    originalTarget_.Reset();
    for (auto& target : copiedTargets_) target.Reset();
    return true;
}

void MenuLayerRenderer::BeforeCopy(ID3D11DeviceContext* context, ID3D11Resource* destination,
    ID3D11Resource* source, bool wholeResource)
{
    if (!HasLayer() || busy_ || compositedEyes_ || !originalTarget_ || !source || !destination || context != context_.Get()) return;
    BusyScope busy(busy_);
    ComPtr<ID3D11Resource> original;
    originalTarget_->GetResource(&original);
    bool trackedSource = source == original.Get();
    bool trackedDestination = destination == original.Get();
    ComPtr<ID3D11RenderTargetView>* empty = nullptr;
    for (auto& target : copiedTargets_) {
        if (!target) { if (!empty) empty = &target; continue; }
        ComPtr<ID3D11Resource> copied; target->GetResource(&copied);
        trackedSource |= source == copied.Get();
        trackedDestination |= destination == copied.Get();
    }
    // An overwrite of a tracked destination by unrelated pixels invalidates the
    // deferred batch too. Restore before the native copy determines visibility.
    if (!trackedSource) {
        if (trackedDestination) { statistics_.lastRejection = "copy-overwrite"; RestoreAllCaptured(context); }
        return;
    }
    if (!wholeResource) { statistics_.lastRejection = "partial-copy"; RestoreAllCaptured(context); return; }
    // A future shader can consume a previously bound view without issuing a
    // new SRV-binding call. Such a copy must contain the native menu pixels.
    if (BoundForShaderRead(context, destination)) {
        statistics_.lastRejection = "copy-to-bound-shader-resource";
        RestoreAllCaptured(context); return;
    }
    if (trackedDestination) return;
    if (!empty) { statistics_.lastRejection = "copy-capacity"; RestoreAllCaptured(context); return; }
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    if (FAILED(destination->QueryInterface(IID_PPV_ARGS(&texture))) ||
        !SimpleTexture(texture.Get(), device_.Get(), desc) || desc.Width != renderWidth_ || desc.Height != renderHeight_ ||
        !(desc.BindFlags & D3D11_BIND_RENDER_TARGET)) { statistics_.lastRejection = "copy-target"; RestoreAllCaptured(context); return; }
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    originalTarget_->GetDesc(&view);
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device_->CreateRenderTargetView(texture.Get(), &view, &target)) ||
        !CanComposite(context, target.Get(), D3D11_VIEWPORT{0, 0, float(renderWidth_), float(renderHeight_), 0, 1})) {
        statistics_.lastRejection = "copy-target"; RestoreAllCaptured(context); return;
    }
    *empty = std::move(target);
}

void MenuLayerRenderer::BeforeClear(ID3D11DeviceContext* context, ID3D11Resource* resource)
{
    if (!HasLayer() || busy_ || compositedEyes_ || context != context_.Get() || !originalTarget_) return;
    BusyScope busy(busy_);
    if (!resource) { statistics_.lastRejection = "unclassified-gpu-work"; RestoreAllCaptured(context); return; }
    ComPtr<ID3D11Resource> original;
    originalTarget_->GetResource(&original);
    if (original.Get() == resource) { statistics_.lastRejection = "resource-access"; RestoreAllCaptured(context); return; }
    for (const auto& target : copiedTargets_) {
        if (!target) continue;
        ComPtr<ID3D11Resource> copied; target->GetResource(&copied);
        if (copied.Get() == resource) { statistics_.lastRejection = "resource-access"; RestoreAllCaptured(context); return; }
    }
}

bool MenuLayerRenderer::RestoreCaptured(ID3D11DeviceContext* context, ID3D11RenderTargetView* target)
{
    if (!HasLayer() || !target || compositedEyes_) return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    target->GetResource(&resource);
    if (FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    if (!DrawLayer(context, target, 2, viewport, false, nullptr, nativeAlphaOver_)) return false;
    captured_ = false;
    ++statistics_.restores;
    sealed_ = true;
    originalTarget_.Reset();
    for (auto& targetView : copiedTargets_) targetView.Reset();
    return true;
}

bool MenuLayerRenderer::CanComposite(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
    const D3D11_VIEWPORT& viewport) const
{
    if (!compositionContext_ || !target || context != context_.Get()) return false;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    target->GetResource(&resource);
    if (resource.Get() == layer_.Get() || FAILED(resource.As(&texture))) return false;
    D3D11_TEXTURE2D_DESC desc{};
    if (!SimpleTexture(texture.Get(), device_.Get(), desc)) return false;
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    target->GetDesc(&view);
    UINT support = 0;
    if (view.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice >= desc.MipLevels ||
        FAILED(device_->CheckFormatSupport(view.Format, &support)) || !(support & D3D11_FORMAT_SUPPORT_BLENDABLE) ||
        !ValidViewport(viewport, std::max(1u, desc.Width >> view.Texture2D.MipSlice),
            std::max(1u, desc.Height >> view.Texture2D.MipSlice))) return false;
    return true;
}

bool MenuLayerRenderer::DrawLayer(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
    int eye, const D3D11_VIEWPORT& viewport, bool flipY, const float* eyeUVBounds, bool restoreAlpha)
{
    if (!CanComposite(context, target, viewport)) return false;
    if (eyeUVBounds && (!std::isfinite(eyeUVBounds[0]) || !std::isfinite(eyeUVBounds[1]) ||
        !std::isfinite(eyeUVBounds[2]) || !std::isfinite(eyeUVBounds[3]) ||
        eyeUVBounds[0] < 0 || eyeUVBounds[1] < 0 || eyeUVBounds[2] > 1 || eyeUVBounds[3] > 1 ||
        eyeUVBounds[0] >= eyeUVBounds[2] || eyeUVBounds[1] >= eyeUVBounds[3])) return false;
    CompositeConstants data{{viewport.TopLeftX, viewport.TopLeftY}, {viewport.Width, viewport.Height},
        {float(layerWidth_), float(layerHeight_)}, UINT(eye), flipY ? 1u : 0u, {0, 0}, {1, 1}};
    if (eyeUVBounds) {
        data.uvOrigin[0] = eyeUVBounds[0]; data.uvOrigin[1] = eyeUVBounds[1];
        data.uvExtent[0] = eyeUVBounds[2] - eyeUVBounds[0];
        data.uvExtent[1] = eyeUVBounds[3] - eyeUVBounds[1];
    }
    // Record on an isolated context. ExecuteCommandList(TRUE) restores the
    // complete immediate-context state, including hidden SO offsets and UAV
    // counters, without changing the application's live context state object.
    auto* recording = compositionContext_.Get();
    recording->ClearState();
    recording->UpdateSubresource(constants_.Get(), 0, nullptr, &data, 0, 0);
    recording->SetPredication(nullptr, FALSE);
    recording->IASetInputLayout(nullptr);
    recording->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    recording->VSSetShader(vertex_.Get(), nullptr, 0);
    recording->PSSetShader(pixel_.Get(), nullptr, 0);
    recording->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    recording->PSSetShaderResources(0, 1, layerSRV_.GetAddressOf());
    recording->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    recording->RSSetState(raster_.Get());
    recording->RSSetViewports(1, &viewport);
    recording->OMSetDepthStencilState(depth_.Get(), 0);
    recording->OMSetBlendState(restoreAlpha ? premultipliedBlend_.Get() : compositeBlend_.Get(), nullptr, UINT(-1));
    recording->OMSetRenderTargets(1, &target, nullptr);
    recording->Draw(3, 0);
    ComPtr<ID3D11CommandList> commands;
    const HRESULT finished = recording->FinishCommandList(FALSE, &commands);
    // Do not retain an acquired runtime image or an old atlas in the recorder,
    // including after a failed command-list allocation.
    recording->ClearState();
    if (FAILED(finished) || !commands) return false;
    context->ExecuteCommandList(commands.Get(), TRUE);
    return true;
}
