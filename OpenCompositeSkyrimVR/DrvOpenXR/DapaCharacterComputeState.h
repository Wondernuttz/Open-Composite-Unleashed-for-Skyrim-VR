#pragma once
#include <d3d11_1.h>
#include <array>

// Predication also suppresses Copy/Update/Clear, not just Draw and Dispatch.
// Use this smaller guard for DAPA's cache and swapchain transfer operations.
class CharacterPredicationState {
    ID3D11DeviceContext* context;
    ID3D11Predicate* predicate = nullptr;
    BOOL value = FALSE;
public:
    explicit CharacterPredicationState(ID3D11DeviceContext* ctx) noexcept : context(ctx) {
        context->GetPredication(&predicate, &value);
        context->SetPredication(nullptr, FALSE);
    }
    CharacterPredicationState(const CharacterPredicationState&) = delete;
    CharacterPredicationState& operator=(const CharacterPredicationState&) = delete;
    ~CharacterPredicationState() noexcept {
        context->SetPredication(predicate, value);
        if (predicate) predicate->Release();
    }
};

// DAPA borrows the game's immediate context. Restore precisely the compute
// bindings its warp dispatch replaces so a producer can resume without rebinding.
class DapaCharacterComputeState {
    ID3D11DeviceContext* context;
    CharacterPredicationState predication;
    ID3D11DeviceContext1* context1 = nullptr;
    ID3D11ComputeShader* shader = nullptr;
    std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{};
    UINT instanceCount = static_cast<UINT>(instances.size());
    std::array<ID3D11ShaderResourceView*, 5> srvs{};
    std::array<ID3D11UnorderedAccessView*, 3> uavs{};
    std::array<ID3D11Buffer*, 2> constants{};
    std::array<UINT, 2> firstConstants{}, numConstants{};
    ID3D11SamplerState* sampler = nullptr;
    UINT uavCount;

    template<class T> static void Release(T* pointer) noexcept {
        if (pointer) pointer->Release();
    }
public:
    explicit DapaCharacterComputeState(ID3D11DeviceContext* ctx, bool capture) noexcept
        : context(ctx), predication(ctx), uavCount(3u)
    {
        context->CSGetShader(&shader, instances.data(), &instanceCount);
        if (!shader) instanceCount = 0;
        context->CSGetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
        context->CSGetUnorderedAccessViews(0, uavCount, uavs.data());
        if (SUCCEEDED(context->QueryInterface(__uuidof(ID3D11DeviceContext1),
                reinterpret_cast<void**>(&context1))))
            context1->CSGetConstantBuffers1(0, 2, constants.data(), firstConstants.data(), numConstants.data());
        else
            context->CSGetConstantBuffers(0, 2, constants.data());
        context->CSGetSamplers(0, 1, &sampler);
    }
    DapaCharacterComputeState(const DapaCharacterComputeState&) = delete;
    DapaCharacterComputeState& operator=(const DapaCharacterComputeState&) = delete;

    ~DapaCharacterComputeState() noexcept {
        // Clear DAPA's reads and writes before restoring bindings: a saved SRV
        // may otherwise conflict with an output still bound by the warp.
        ID3D11ShaderResourceView* noSrvs[5]{};
        ID3D11UnorderedAccessView* noUavs[3]{};
        context->CSSetShaderResources(0, 5, noSrvs);
        context->CSSetUnorderedAccessViews(0, uavCount, noUavs, nullptr);
        // UINT(-1) preserves append/consume UAV counters; restoring a view must
        // not restart an external producer's append position.
        const UINT keepCounts[3] = { UINT(-1), UINT(-1), UINT(-1) };
        context->CSSetUnorderedAccessViews(0, uavCount, uavs.data(), keepCounts);
        context->CSSetShaderResources(0, 5, srvs.data());
        if (context1)
            context1->CSSetConstantBuffers1(0, 2, constants.data(), firstConstants.data(), numConstants.data());
        else
            context->CSSetConstantBuffers(0, 2, constants.data());
        context->CSSetSamplers(0, 1, &sampler);
        context->CSSetShader(shader, instances.data(), instanceCount);

        Release(shader);
        for (auto* instance : instances) Release(instance);
        for (auto* srv : srvs) Release(srv);
        for (auto* uav : uavs) Release(uav);
        for (auto* buffer : constants) Release(buffer);
        Release(sampler);
        Release(context1);
    }
};
