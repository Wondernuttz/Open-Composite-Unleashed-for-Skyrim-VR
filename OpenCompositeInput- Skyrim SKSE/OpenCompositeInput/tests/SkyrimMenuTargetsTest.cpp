#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../../OpenCompositeSkyrimVR/OpenOVR/Compositor/SkyrimMenuTargets.h"
using Microsoft::WRL::ComPtr;
#include "SkyrimMenuTargetsProduction.inl"

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #value); std::exit(1); } } while (false)

static ComPtr<ID3D11Device> Device(ComPtr<ID3D11DeviceContext>& context)
{
    ComPtr<ID3D11Device> device;
    CHECK(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    return device;
}

static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device)
{
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 16;
    description.Height = 8;
    description.MipLevels = description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    CHECK(SUCCEEDED(device->CreateTexture2D(&description, nullptr, &texture)));
    return texture;
}

static void Reject(const ocu_menu::Targets& candidate, ID3D11Device* device,
    ocu_menu::Targets output = {})
{
    const auto before = output;
    CHECK(!CommitTargets(candidate, device, output));
    CHECK(std::memcmp(&before, &output, sizeof(output)) == 0);
}

int main()
{
    ComPtr<ID3D11DeviceContext> context, otherContext, deferred;
    auto device = Device(context);
    auto otherDevice = Device(otherContext);
    auto source = Texture(device.Get());
    auto destination = Texture(device.Get());
    auto foreign = Texture(otherDevice.Get());

    ocu_menu::Targets candidate;
    candidate.context = context.Get();
    candidate.sources[0] = source.Get();
    candidate.sources[1] = source.Get(); // Each occupied slot owns a reference.
    candidate.destinations[0] = destination.Get();
    candidate.destinations[1] = destination.Get();

    auto malformed = ocu_menu::Targets{};
    malformed.size -= 1;
    Reject(candidate, device.Get(), malformed);
    malformed = {};
    malformed.version += 1;
    Reject(candidate, device.Get(), malformed);
    malformed = {};
    malformed.sources[3] = source.Get();
    Reject(candidate, device.Get(), malformed);

    auto changed = candidate;
    changed.context = nullptr;
    Reject(changed, device.Get());
    CHECK(SUCCEEDED(device->CreateDeferredContext(0, &deferred)));
    changed.context = deferred.Get();
    Reject(changed, device.Get());
    Reject(candidate, otherDevice.Get());
    Reject(candidate, nullptr);
    changed = candidate;
    changed.sources[0] = foreign.Get();
    Reject(changed, device.Get());
    changed = candidate;
    changed.destinations[0] = foreign.Get();
    Reject(changed, device.Get());
    changed = candidate;
    changed.destinations[0] = source.Get();
    Reject(changed, device.Get());
    changed = candidate;
    for (auto*& texture : changed.sources) texture = nullptr;
    Reject(changed, device.Get());
    changed = candidate;
    for (auto*& texture : changed.destinations) texture = nullptr;
    Reject(changed, device.Get());

    ocu_menu::Targets output;
    CHECK(CommitTargets(candidate, device.Get(), output));
    CHECK(output.context == context.Get());
    CHECK(output.sources[0] == source.Get() && output.sources[1] == source.Get());
    CHECK(output.destinations[0] == destination.Get());
    CHECK(!output.sources[2] && !output.destinations[2]);

    // Returned references must outlive the original texture owners, and each
    // repeated slot must remain independently releasable.
    source.Reset();
    destination.Reset();
    for (auto*& texture : output.sources) if (texture) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        CHECK(description.Width == 16 && description.Height == 8);
        texture->Release();
        texture = nullptr;
    }
    for (auto*& texture : output.destinations) if (texture) {
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        CHECK(description.Width == 16 && description.Height == 8);
        texture->Release();
        texture = nullptr;
    }
    CHECK(output.context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
    output.context->Release();
    std::puts("Skyrim menu target ownership and rejection checks passed.");
}
