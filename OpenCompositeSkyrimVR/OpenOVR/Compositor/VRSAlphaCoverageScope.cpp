#include "VRSAlphaCoverageScope.h"
#include "RDMRenderScope.h"
#include "VRSShaderGuard.h"
#include <wrl/client.h>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

struct VRSAlphaCoverageScope::Impl {
    ID3D11DeviceContext* context = nullptr;
    ComPtr<ID3D11Texture2D> sceneDepth;
    std::unordered_map<ID3D11Resource*, ComPtr<ID3D11Resource>> materials;
    ComPtr<ID3D11Resource> currentMaterial, pendingMaterial;
    std::vector<ComPtr<ID3D11Resource>> sampledMaterials;
    ocu_vrs_guard::SampledTexture2DSlots sampledSlots{};
    ProtectionChanged changed = nullptr;
    RDMRenderScope::DrawObserver observer{};
    Statistics stats{};
    bool dirty = true, armed = false, protect = false, canRecord = false, equalColor = false;
    bool commandList = false, ambiguousCoverage = false, unknownDepthMaterial = false, untrackedDepthMaterial = false;
    unsigned reasons = ocu_vrs_guard::Unclassified;
    unsigned coarseHazards = ocu_vrs_guard::CoarseUnclassified;
    int materialSlot = -1;

    Impl()
    {
        sampledMaterials.reserve(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
        observer.owner = this;
        observer.beforeDraw = [](void* p, ID3D11DeviceContext*) { static_cast<Impl*>(p)->BeforeDraw(); };
        observer.afterDraw = [](void* p, ID3D11DeviceContext*) { static_cast<Impl*>(p)->AfterDraw(); };
        observer.stateChanged = [](void* p, ID3D11DeviceContext*) { static_cast<Impl*>(p)->dirty = true; };
        observer.depthCleared = [](void* p, ID3D11DeviceContext*, ID3D11DepthStencilView* view) {
            auto& self = *static_cast<Impl*>(p);
            ComPtr<ID3D11Resource> resource;
            if (view) view->GetResource(&resource);
            D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
            if (view) view->GetDesc(&desc);
            if (resource.Get() == self.sceneDepth.Get() &&
                desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && desc.Texture2D.MipSlice == 0 &&
                !(desc.Flags & D3D11_DSV_READ_ONLY_DEPTH)) {
                self.materials.clear(); self.pendingMaterial.Reset();
                self.stats.materials = 0; ++self.stats.resets;
                // A complete depth clear removes unknown command-list coverage.
                if (!self.commandList) self.ambiguousCoverage = false;
            }
            self.dirty = true;
        };
    }

    void Query()
    {
        ++stats.stateQueries;
        dirty = false; canRecord = equalColor = unknownDepthMaterial = untrackedDepthMaterial = false; currentMaterial.Reset(); sampledMaterials.clear();
        reasons = ocu_vrs_guard::CurrentReasons(context);
        coarseHazards = ocu_vrs_guard::CurrentCoarseHazards(context);
        materialSlot = ocu_vrs_guard::CurrentSingleSampledTexture2D(context);
        sampledSlots = ocu_vrs_guard::CurrentSampledTexture2DSlots(context);
        if (commandList || (reasons & ocu_vrs_guard::CommandList)) return;
        ComPtr<ID3D11DepthStencilView> dsv;
        context->OMGetRenderTargets(0, nullptr, &dsv);
        if (!dsv) return;
        ComPtr<ID3D11Resource> depthResource; dsv->GetResource(&depthResource);
        if (depthResource.Get() != sceneDepth.Get()) return;
        D3D11_DEPTH_STENCIL_VIEW_DESC view{}; dsv->GetDesc(&view);
        if (view.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || view.Texture2D.MipSlice != 0) return;
        ComPtr<ID3D11DepthStencilState> depthState;
        context->OMGetDepthStencilState(&depthState, nullptr);
        D3D11_DEPTH_STENCIL_DESC depth{};
        if (depthState) depthState->GetDesc(&depth);
        else { depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc = D3D11_COMPARISON_LESS; }
        if (!depth.DepthEnable) return;
        equalColor = depth.DepthFunc == D3D11_COMPARISON_EQUAL;
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
            if (!sampledSlots.Contains(slot)) continue;
            ComPtr<ID3D11ShaderResourceView> srv;
            context->PSGetShaderResources(slot, 1, &srv);
            if (!srv) continue; // includes D3D's implicit unbind on resource hazards
            D3D11_SHADER_RESOURCE_VIEW_DESC sampledView{}; srv->GetDesc(&sampledView);
            if (sampledView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) continue;
            ComPtr<ID3D11Resource> resource; srv->GetResource(&resource);
            if (int(slot) == materialSlot) currentMaterial = resource;
            sampledMaterials.push_back(std::move(resource));
        }
        if (!(reasons & ocu_vrs_guard::Discard) ||
            depth.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL || depth.DepthFunc == D3D11_COMPARISON_NEVER ||
            (view.Flags & D3D11_DSV_READ_ONLY_DEPTH)) return;
        unknownDepthMaterial = (coarseHazards & ocu_vrs_guard::CoarseUnclassified) != 0;
        if (!currentMaterial) { untrackedDepthMaterial = true; return; }
        // Predication/stencil/indirect arguments may produce zero pixels. A
        // submitted cutout depth draw still conservatively marks its material;
        // do not read GPU query results or miss a predicate that actually passes.
        canRecord = true;
    }

    void BeforeDraw()
    {
        if (!armed) return;
        if (dirty) Query();
        pendingMaterial = canRecord ? currentMaterial : nullptr;
        if (untrackedDepthMaterial) ++stats.untrackedDepthDraws;
        if (unknownDepthMaterial) ambiguousCoverage = true;
        bool sampledCutout = false;
        for (const auto& material : sampledMaterials)
            sampledCutout = sampledCutout || materials.find(material.Get()) != materials.end();
        const bool wasProtected = protect;
        protect = equalColor && (ambiguousCoverage || sampledCutout);
        if (protect) {
            ++stats.protectedDraws;
            if (ambiguousCoverage) ++stats.ambiguousDraws;
        }
        if (protect != wasProtected && changed) changed(context);
    }

    void AfterDraw()
    {
        if (!armed) return;
        if (pendingMaterial) {
            ++stats.depthDraws;
            // Bound frame-local bookkeeping and fail closed if allocation fails.
            // Existing references prevent COM identity reuse until EndFrame.
            if (materials.size() >= 4096 && materials.find(pendingMaterial.Get()) == materials.end())
                ambiguousCoverage = true;
            else try { materials.emplace(pendingMaterial.Get(), pendingMaterial); }
            catch (const std::bad_alloc&) { ambiguousCoverage = true; }
            stats.materials = unsigned(materials.size());
            pendingMaterial.Reset();
        }
        // Keep the same protection through consecutive grass draws. The next
        // BeforeDraw restores coarse shading before any ordinary material draw.
    }
};

VRSAlphaCoverageScope::VRSAlphaCoverageScope() : impl(std::make_unique<Impl>()) {}
VRSAlphaCoverageScope::~VRSAlphaCoverageScope() { EndFrame(); }
bool VRSAlphaCoverageScope::Arm(ID3D11DeviceContext* context, ID3D11Texture2D* depth, ProtectionChanged callback)
{
    EndFrame();
    if (!context || !depth || !callback || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    auto& p = *impl;
    p.context = context; p.sceneDepth = depth; p.changed = callback; p.stats = {};
    p.dirty = true; p.commandList = p.ambiguousCoverage = false;
    p.reasons = ocu_vrs_guard::Unclassified; p.materialSlot = -1;
    if (!RDMRenderScope::RegisterDrawObserver(context, &p.observer)) { EndFrame(); return false; }
    p.armed = true;
    return true;
}
void VRSAlphaCoverageScope::EndFrame()
{
    auto& p = *impl;
    if (p.context) RDMRenderScope::RemoveDrawObserver(p.context);
    // No callback here: disarming must never re-enable VRS after the owner disabled it.
    p.armed = p.protect = false; p.context = nullptr; p.changed = nullptr;
    p.sceneDepth.Reset(); p.currentMaterial.Reset(); p.pendingMaterial.Reset(); p.materials.clear();
    p.sampledMaterials.clear();
    p.canRecord = p.equalColor = p.commandList = p.ambiguousCoverage = false; p.dirty = true;
}
void VRSAlphaCoverageScope::ShaderStateChanged(ID3D11DeviceContext* context, bool bindingsReplaced)
{
    auto& p = *impl;
    if (!p.armed || context != p.context) return;
    const auto reasons = ocu_vrs_guard::CurrentReasons(context);
    const auto coarseHazards = ocu_vrs_guard::CurrentCoarseHazards(context);
    const int slot = ocu_vrs_guard::CurrentSingleSampledTexture2D(context);
    const auto sampled = ocu_vrs_guard::CurrentSampledTexture2DSlots(context);
    bool sampledChanged = false;
    for (unsigned i = 0; i < 4; ++i) sampledChanged = sampledChanged || sampled.words[i] != p.sampledSlots.words[i];
    const bool command = (reasons & ocu_vrs_guard::CommandList) != 0;
    if (command && !p.commandList) {
        ++p.stats.unknownCommandLists;
        // Native command-list playback bypasses CPU draw hooks. Its possible
        // alpha depth cannot be learned: protect equal-depth colors this frame.
        p.ambiguousCoverage = true;
        p.materials.clear(); p.stats.materials = 0; p.pendingMaterial.Reset();
    }
    if (bindingsReplaced) {
        // ClearState and swaps replace cached bindings; previously observed
        // cutout material marks remain conservative until scene depth is cleared.
        p.currentMaterial.Reset(); p.pendingMaterial.Reset(); p.sampledMaterials.clear();
    }
    if (bindingsReplaced || command != p.commandList || reasons != p.reasons || coarseHazards != p.coarseHazards || slot != p.materialSlot || sampledChanged)
        p.dirty = true;
    p.commandList = command; p.reasons = reasons; p.materialSlot = slot;
    p.coarseHazards = coarseHazards;
    p.sampledSlots = sampled;
}
bool VRSAlphaCoverageScope::ProtectsCurrentDraw(ID3D11DeviceContext* context) const
{ return impl->armed && impl->context == context && impl->protect; }
VRSAlphaCoverageScope::Statistics VRSAlphaCoverageScope::Stats() const { return impl->stats; }
