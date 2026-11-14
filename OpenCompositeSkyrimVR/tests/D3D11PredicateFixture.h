#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace ocu_test {
inline Microsoft::WRL::ComPtr<ID3D11Predicate> EmptyOcclusionPredicate(
    ID3D11Device* device, ID3D11DeviceContext* context)
{
    const auto fail = [](HRESULT result) {
        char message[80];
        std::snprintf(message, sizeof(message), "Occlusion predicate fixture failed (HRESULT 0x%08X)", unsigned(result));
        throw std::runtime_error(message);
    };
    const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
    const HRESULT created = device->CreatePredicate(&desc, &predicate);
    if (FAILED(created)) fail(created);
    context->Begin(predicate.Get());
    context->End(predicate.Get());
    context->Flush();

    // Sleep granularity varies; bound the wait by elapsed time, not poll count.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        BOOL visible = TRUE;
        const HRESULT ready = context->GetData(predicate.Get(), &visible, sizeof(visible), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (ready == S_OK) {
            if (visible != FALSE) throw std::runtime_error("Empty occlusion predicate did not resolve false");
            return predicate;
        }
        if (ready != S_FALSE) fail(ready);
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error("Empty occlusion predicate timed out after 10 seconds");
        Sleep(1);
    }
}
} // namespace ocu_test
