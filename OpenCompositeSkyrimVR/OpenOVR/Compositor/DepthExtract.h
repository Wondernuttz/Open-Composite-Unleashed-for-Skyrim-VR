#pragma once
#include <d3d11.h>
#include <wrl/client.h>

namespace ocu_depth_extract {
inline constexpr char Shader[] = R"HLSL(
Texture2D<float>   DepthIn  : register(t0);  // R24_UNORM_X8_TYPELESS view of depth-stencil
RWTexture2D<float> DepthOut : register(u0);  // R32_FLOAT output

[numthreads(8, 8, 1)]
void CS_DepthExtract(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    DepthOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    DepthOut[id.xy] = DepthIn.Load(int3(id.xy, 0));
}
)HLSL";

// Extraction runs on Skyrim's immediate context. A writable DSV of the
// input texture would make CSSetShaderResources bind NULL instead of depth.
// Detach just that DSV, keep all OM UAVs/counters, and restore every state
// changed by this dispatch. No copies, extra dispatches, or GPU waits.
class ScopedState {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    ID3D11DeviceContext* context;
    Ptr<ID3D11ComputeShader> shader;
    ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
    UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
    Ptr<ID3D11ShaderResourceView> source;
    Ptr<ID3D11UnorderedAccessView> output;
    Ptr<ID3D11DepthStencilView> depth;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    UINT targetCount = 0;
    bool detached = false;
public:
    ScopedState(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* input) : context(ctx)
    {
        context->CSGetShader(&shader, instances, &instanceCount);
        if (!shader) instanceCount = 0;
        context->CSGetShaderResources(0, 1, &source);
        context->CSGetUnorderedAccessViews(0, 1, &output);
        context->OMGetRenderTargets(0, nullptr, &depth);
        if (!depth) return;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        depth->GetDesc(&view);
        if (view.Flags & D3D11_DSV_READ_ONLY_DEPTH) return;
        Ptr<ID3D11Resource> boundResource, inputResource;
        depth->GetResource(&boundResource);
        input->GetResource(&inputResource);
        if (boundResource != inputResource) return;

        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets, nullptr);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            if (targets[i]) targetCount = i + 1;
        // KEEP_UAVS still unbinds UAV slots below NumRTVs. Rebinding all eight
        // slots would therefore erase live game UAVs when fewer RTVs are used.
        context->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, targets, nullptr,
            0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        detached = true;
    }
    ScopedState(const ScopedState&) = delete;
    ScopedState& operator=(const ScopedState&) = delete;
    ~ScopedState()
    {
        ID3D11ShaderResourceView* emptySource = nullptr;
        ID3D11UnorderedAccessView* emptyOutput = nullptr;
        context->CSSetShaderResources(0, 1, &emptySource);
        context->CSSetUnorderedAccessViews(0, 1, &emptyOutput, nullptr);
        if (detached)
            context->OMSetRenderTargetsAndUnorderedAccessViews(targetCount, targets, depth.Get(),
                0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
        context->CSSetShader(shader.Get(), instances, instanceCount);
        auto* originalOutput = output.Get();
        context->CSSetUnorderedAccessViews(0, 1, &originalOutput, nullptr);
        auto* originalSource = source.Get();
        context->CSSetShaderResources(0, 1, &originalSource);
        for (auto* target : targets) if (target) target->Release();
        for (UINT i = 0; i < instanceCount; ++i) if (instances[i]) instances[i]->Release();
    }
};

inline bool Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
    ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* output, UINT width, UINT height)
{
    if (!context || !shader || !source || !output || !width || !height) return false;
    ScopedState saved(context, source);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}
} // namespace ocu_depth_extract
