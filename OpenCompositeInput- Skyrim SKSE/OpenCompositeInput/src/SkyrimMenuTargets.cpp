// CommonLib's REX declarations must precede the Windows SDK headers.
#include <RE/R/Renderer.h>
#include <REL/Module.h>
#include <SKSE/Version.h>

#include <d3d11.h>
#include <wrl/client.h>

#include "../../../OpenCompositeSkyrimVR/OpenOVR/Compositor/SkyrimMenuTargets.h"

#include "MenuSeparationPolicy.h"

#include <cstddef>
#include <cstring>

namespace {
using Microsoft::WRL::ComPtr;

// CommonLib documents the VR renderer layout but its RendererData array is
// shorter than the VR-only target IDs. Read a full, version-gated physical view
// without indexing beyond that declared member. Do not modify the dependency.
constexpr std::size_t VrTargetsOffset = 0xA60;
struct VrRenderTargets {
    RE::BSGraphics::RenderTargetData entries[RE::RENDER_TARGET::kVRTOTAL];
};
static_assert(sizeof(RE::BSGraphics::RenderTargetData) == 0x30);
static_assert(offsetof(RE::BSGraphics::RendererData, renderTargets) == 0xA48);
static_assert(RE::RENDER_TARGET::kPROJECTEDMENU == 115);
static_assert(RE::RENDER_TARGET::kHUDMENU == 116);
static_assert(RE::RENDER_TARGET::kVRTOTAL == 125);
static_assert(VrTargetsOffset + sizeof(VrRenderTargets) == 0x21D0);

RE::BSGraphics::RenderTargetData ReadTarget(
    const RE::BSGraphics::Renderer* renderer, RE::RENDER_TARGET target) noexcept
{
    RE::BSGraphics::RenderTargetData result{};
    const auto index = static_cast<std::size_t>(target);
    if (index >= RE::RENDER_TARGET::kVRTOTAL) return result;
    const auto* address = reinterpret_cast<const std::byte*>(renderer) +
        VrTargetsOffset + index * sizeof(result);
    std::memcpy(&result, address, sizeof(result));
    return result;
}

bool EmptyRequest(const ocu_menu::Targets& request) noexcept
{
    if (request.size != sizeof(request) || request.version != ocu_menu::Version || request.context)
        return false;
    for (auto* texture : request.sources) if (texture) return false;
    for (auto* texture : request.destinations) if (texture) return false;
    return true;
}

bool ValidTexture(ID3D11Texture2D* texture, ID3D11Device* device) noexcept
{
    if (!texture) return true;
    ComPtr<ID3D11Device> owner;
    texture->GetDevice(&owner);
    if (owner.Get() != device) return false;
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    return description.Width && description.Height && description.MipLevels &&
        description.ArraySize && description.SampleDesc.Count;
}

bool ValidateTargets(const ocu_menu::Targets& candidate, ID3D11Device* rendererDevice) noexcept
{
    if (!candidate.context || !rendererDevice ||
        candidate.context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        return false;
    ComPtr<ID3D11Device> contextDevice;
    candidate.context->GetDevice(&contextDevice);
    if (contextDevice.Get() != rendererDevice) return false;

    bool hasSource = false, hasDestination = false;
    for (auto* texture : candidate.sources) {
        if (!ValidTexture(texture, rendererDevice)) return false;
        hasSource |= texture != nullptr;
        if (texture) for (auto* destination : candidate.destinations)
            if (texture == destination) return false;
    }
    for (auto* texture : candidate.destinations) {
        if (!ValidTexture(texture, rendererDevice)) return false;
        hasDestination |= texture != nullptr;
    }
    return hasSource && hasDestination;
}

bool CommitTargets(const ocu_menu::Targets& candidate, ID3D11Device* device,
    ocu_menu::Targets& output) noexcept
{
    if (!EmptyRequest(output) || !ValidateTargets(candidate, device)) return false;
    candidate.context->AddRef();
    for (auto* texture : candidate.sources) if (texture) texture->AddRef();
    for (auto* texture : candidate.destinations) if (texture) texture->AddRef();
    output = candidate;
    return true;
}
} // namespace

extern "C" __declspec(dllexport) bool __cdecl OCU_CanSeparateMenu() noexcept
{
    return ocu_menu_policy::Allowed();
}

extern "C" __declspec(dllexport) bool __cdecl OCU_AcquireMenuTargets(ocu_menu::Targets* output) noexcept
{
    if (!ocu_menu_policy::Allowed() || !output || !EmptyRequest(*output)) return false;
    try {
        if (!REL::Module::IsVR() || REL::Module::get().version() != SKSE::RUNTIME_VR_1_4_15)
            return false;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return false;
        auto& runtime = renderer->GetRuntimeData();
        auto* device = reinterpret_cast<ID3D11Device*>(runtime.forwarder);
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(runtime.context);
        if (!device || !context) return false;

        const auto projected = ReadTarget(renderer, RE::RENDER_TARGET::kPROJECTEDMENU);
        const auto hud = ReadTarget(renderer, RE::RENDER_TARGET::kHUDMENU);
        const auto menuBackground = ReadTarget(renderer, RE::RENDER_TARGET::kMENUBG);
        ocu_menu::Targets candidate;
        candidate.context = context;
        candidate.sources[0] = projected.texture;
        candidate.sources[1] = projected.textureCopy;
        candidate.sources[2] = hud.texture;
        candidate.sources[3] = hud.textureCopy;
        candidate.destinations[0] = menuBackground.texture;
        candidate.destinations[1] = menuBackground.textureCopy;
        // v1 intentionally publishes only the documented paused-menu target.
        // Main/loading and world-space UI destinations keep the native path.
        // The caller runs on Skyrim's render thread, so renderer recreation
        // cannot interleave with this snapshot. No references escape on failure.
        return CommitTargets(candidate, device, *output);
    } catch (...) {
        return false;
    }
}
