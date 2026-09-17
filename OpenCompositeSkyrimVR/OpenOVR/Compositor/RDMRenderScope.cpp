#include "RDMRenderScope.h"
#include "RDMGpuTiming.h"
#include <chrono>
#include "VRSShaderGuard.h"
#include "VRSSceneScope.h"
#include "../logging.h"
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>
#include <cstring>
#include <type_traits>
#include <tuple>

using Microsoft::WRL::ComPtr;
namespace {
std::atomic<RDMRenderScope*> active{nullptr};
std::atomic<ID3D11DeviceContext*> activeContext{nullptr};
std::atomic<void*> externalTargets{nullptr}, externalTargetsUav{nullptr};
std::atomic<ID3D11DeviceContext*> drawObserverContext{nullptr};
const RDMRenderScope::DrawObserver* drawObserver = nullptr;
thread_local unsigned observerDrawDepth = 0;
thread_local ID3D11DeviceContext* nativeDrawContext = nullptr;
const RDMRenderScope::DrawObserver* Observer(ID3D11DeviceContext* ctx)
{
    if (ctx != drawObserverContext.load(std::memory_order_acquire) ||
        ctx == activeContext.load(std::memory_order_acquire)) return nullptr;
    return drawObserver;
}
constexpr char depthEligibilityShader[] = R"HLSL(
Texture2D<float> Depth : register(t0);
Texture2D<uint2> Guide : register(t1);
RWTexture2D<float> Eligible : register(u0);
cbuffer Parameters : register(b0) { uint2 origin; uint2 size; uint clusterOffset; float guideDepthTolerance; uint2 padding; };
groupshared uint minimumDepth, maximumDepth, minimumOwner, maximumOwner, invalidOwner;
[numthreads(8,8,1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID, uint index : SV_GroupIndex) {
    if (index == 0) { minimumDepth = asuint(1.0); maximumDepth = 0; minimumOwner=0xffffffffu; maximumOwner=0; invalidOwner=0; }
    GroupMemoryBarrierWithGroupSync();
    uint2 p = group.xy * 8 + lane.xy;
    float z = all(p < size) ? Depth.Load(int3(origin+p,0)) : 1.0;
    uint2 guide = all(p<size) ? Guide.Load(int3(origin+p,0)) : uint2(0,0);
    InterlockedMin(minimumDepth, asuint(z)); InterlockedMax(maximumDepth, asuint(z));
    InterlockedMin(minimumOwner,guide.x); InterlockedMax(maximumOwner,guide.x);
    if(!guide.x || abs(asfloat(guide.y)-z)>guideDepthTolerance)InterlockedOr(invalidOwner,1);
    GroupMemoryBarrierWithGroupSync();
    if (index == 0) {
        float lo = asfloat(minimumDepth), hi = asfloat(maximumDepth);
        float tolerance = max(1e-7, min(lo, 1.0-hi) * 0.01);
        Eligible[group.xy + uint2(clusterOffset,0)] = !invalidOwner && minimumOwner==maximumOwner &&
            lo > 0 && hi < 1 && hi-lo <= tolerance ? 1.0 : 0.0;
    }
})HLSL";
template<class T> void Name(T* object, const char* name)
{
    if (object) object->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(std::strlen(name)), name);
}
struct DrawHandler;
template<class Function, class Handler> struct Hook;
void ReportHookFailure(const char* stage, UINT slot, const void* target, long status)
{
    static std::atomic<unsigned> failures{0};
    if (failures.fetch_add(1, std::memory_order_relaxed) < 16)
        OOVR_LOGF("RDM hook preparation failed: stage=%s slot=%u target=%p status=%ld", stage, slot, target, status);
}
template<class Handler, class Result, class... Args>
struct Hook<Result(STDMETHODCALLTYPE*)(Args...), Handler> {
    using Function = Result(STDMETHODCALLTYPE*)(Args...);
    static inline std::array<void*, 8> targets{};
    static inline std::array<Function, 8> originals{};
    static inline std::array<std::atomic<bool>, 8> forwardOnly{};
    static inline std::mutex lock;
    template<size_t I> static Result STDMETHODCALLTYPE Detour(Args... args)
    {
        if constexpr (std::is_same_v<Handler, DrawHandler>) {
            // Cold D3D11 entries perform native setup, then forward to another
            // draw entry. The inner hook must own masking and observer scope
            // so CSX's accepted-draw callback runs outside RDM's busy scope.
            // Queries expose game state; output setters restore its binding.
            if (forwardOnly[I].load(std::memory_order_relaxed)) return originals[I](args...);
        }
        return Handler::Call(originals[I], args...);
    }
    template<size_t... I> static auto Detours(std::index_sequence<I...>)
    { return std::array<Function, sizeof...(I)>{&Detour<I>...}; }
    static bool Install(void* target, UINT slot, bool forwarding = false)
    {
        std::lock_guard<std::mutex> guard(lock);
        for (size_t i = 0; i < targets.size(); ++i) if (targets[i] == target) {
            if (forwardOnly[i].load(std::memory_order_relaxed) != forwarding) {
                // A target observed as an inner draw on another device/flags
                // combination cannot safely become a forwarding-only hook.
                ReportHookFailure("draw-role-conflict", slot, target, forwarding); return false;
            }
            return true;
        }
        auto detours = Detours(std::make_index_sequence<8>{});
        for (size_t i = 0; i < targets.size(); ++i) if (!targets[i]) {
            const auto created = MH_CreateHook(target, reinterpret_cast<void*>(detours[i]),
                reinterpret_cast<void**>(&originals[i]));
            if (created != MH_OK) { ReportHookFailure("create", slot, target, created); return false; }
            forwardOnly[i].store(forwarding, std::memory_order_relaxed);
            const auto enabled = MH_EnableHook(target);
            if (enabled != MH_OK) {
                ReportHookFailure("enable", slot, target, enabled);
                MH_RemoveHook(target); originals[i] = nullptr; return false;
            }
            targets[i] = target; return true;
        }
        ReportHookFailure("method-capacity", slot, target, long(targets.size()));
        return false;
    }
};
struct DrawHandler {
    template<class F, class... A> static void Call(F original, ID3D11DeviceContext* ctx, A... args)
    {
        // A protected D3D11 entry can delegate to its unprotected variant.
        // Both are hooked; process that one native draw exactly once.
        if (nativeDrawContext == ctx) { original(ctx, args...); return; }
        struct DrawContextScope {
            ID3D11DeviceContext* previous = nativeDrawContext;
            explicit DrawContextScope(ID3D11DeviceContext* context) { nativeDrawContext = context; }
            ~DrawContextScope() { nativeDrawContext = previous; }
        } drawContextScope(ctx);
        auto* owner = RDMRenderScope::Active(ctx);
        bool hasWork = true;
        if constexpr (sizeof...(A) > 0) {
            const auto values = std::forward_as_tuple(args...);
            if constexpr (std::is_integral_v<std::remove_reference_t<decltype(std::get<0>(values))>>) {
                hasWork = std::get<0>(values) != 0;
                if constexpr (sizeof...(A) >= 4) hasWork = hasWork && std::get<1>(values) != 0;
            }
        }
        if (!hasWork) { original(ctx, args...); return; }
        const auto* observer = hasWork && !observerDrawDepth ? Observer(ctx) : nullptr;
        if (observer) {
            ++observerDrawDepth;
            if (observer->beforeDraw) observer->beforeDraw(observer->owner, ctx);
        }
        if (owner) {
            std::array<std::uint64_t, 8> key{};
            // Direct D3D11 draw methods have distinct argument counts (2, 3,
            // 4, 5). Use the operation, not its trampoline address: native
            // query/lazy-dispatch transitions can change that address without
            // changing geometry. Indirect/DrawAuto never reuse this key.
            key[0] = sizeof...(A);
            unsigned index = 1;
            auto append = [&](auto value) {
                if constexpr (std::is_pointer_v<decltype(value)>)
                    key[index++] = reinterpret_cast<std::uintptr_t>(value);
                else key[index++] = static_cast<std::uint64_t>(value);
            };
            (append(args), ...);
            // Indirect arguments and DrawAuto can change without a setter.
            constexpr bool reusable = sizeof...(A) > 0 && (std::is_integral_v<A> && ...);
            if (owner->BeginColorCoverage(key, reusable)) {
                original(ctx, args...);
                owner->EndColorCoverage();
            }
        }
        const bool masked = owner && owner->BeforeDraw();
        const bool guide = owner && !masked && owner->BeginDepthGuide(false);
        if (owner && !guide) owner->BeginSceneDraw(masked);
        original(ctx, args...);
        if (owner && !guide) owner->EndSceneDraw(masked);
        if (guide) owner->EndDepthGuide();
        else if (owner) {
            owner->AfterDraw(masked);
            if (!masked && owner->BeginDepthGuide(true)) {
                original(ctx,args...);
                owner->EndDepthGuide();
            }
        }
        if (observer) {
            if (observer->afterDraw) observer->afterDraw(observer->owner, ctx);
            --observerDrawDepth;
        }
    }
};
template<unsigned GuideFlags> struct StateHandler {
    template<class F, class Context, class... A> static void Call(F original, Context* ctx, A... args)
    {
        if constexpr (GuideFlags & RDMRenderScope::GuideTargets)
            RDMRenderScope::NotifyBeforeDepthStateBoundary(ctx);
        original(ctx, args...);
        RDMRenderScope::NotifyState(ctx, GuideFlags);
    }
};
struct DepthBindingQueryHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        UINT count, ID3D11RenderTargetView** views, ID3D11DepthStencilView** depth)
    {
        original(ctx, count, views, depth);
        if (auto* owner = RDMRenderScope::Active(ctx)) owner->ExposeOriginalDepthBinding(depth);
    }
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        UINT count, ID3D11RenderTargetView** views, ID3D11DepthStencilView** depth,
        UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView** uavs)
    {
        original(ctx, count, views, depth, uavStart, uavCount, uavs);
        if (auto* owner = RDMRenderScope::Active(ctx)) owner->ExposeOriginalDepthBinding(depth);
    }
};
struct GeometryStateHandler {
    template<class F, class Context, class... A> static void Call(F original, Context* ctx, A... args)
    {
        original(ctx, args...);
        RDMRenderScope::NotifyGeometryState(ctx);
    }
};
struct ReadHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx, UINT slot,
        UINT count, ID3D11ShaderResourceView* const* views)
    {
        if (auto* owner = RDMRenderScope::Active(ctx)) owner->BeforeReads(count, views);
        original(ctx, slot, count, views);
        RDMRenderScope::NotifyGeometryState(ctx);
        if (const auto* observer = Observer(ctx); observer && observer->stateChanged)
            observer->stateChanged(observer->owner, ctx);
    }
};
struct ComputeHandler {
    template<class F, class Context, class... A> static void Call(F original, Context* ctx, A... args)
    {
        if (auto* owner = RDMRenderScope::Active(ctx)) owner->BeforeCompute();
        original(ctx, args...);
    }
};
struct ResourceWriteHandler {
    template<class F,class Context,class Resource,class... A>
    static void Call(F original,Context* ctx,Resource* destination,A... args)
    {
        if(auto* owner=RDMRenderScope::Active(ctx)) {
            owner->BeforeCompute();
            if constexpr(std::is_same_v<Resource,ID3D11View>) {
                ComPtr<ID3D11Resource> resource;
                if(destination)destination->GetResource(&resource);
                owner->BeforeWrite(resource.Get());
            } else owner->BeforeWrite(destination);
        }
        original(ctx,destination,args...);
    }
};
struct CopyHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) { o->BeforeRead(src); o->BeforeWrite(dst); }
        original(ctx, dst, src);
    }
};
struct CopyRegionHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx, ID3D11Resource* dst,
        UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) { o->BeforeRead(src); o->BeforeWrite(dst); }
        original(ctx, dst, ds, x, y, z, src, ss, box);
    }
};
struct ResolveHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT format)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) { o->BeforeRead(src); o->BeforeWrite(dst); }
        original(ctx, dst, ds, src, ss, format);
    }
};
struct MapHandler {
    template<class F> static HRESULT Call(F original, ID3D11DeviceContext* ctx,
        ID3D11Resource* resource, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* map)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) {
            if(type==D3D11_MAP_READ)o->BeforeRead(resource);else o->BeforeWrite(resource);
        }
        return original(ctx, resource, sub, type, flags, map);
    }
};
struct UpdateHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        ID3D11Resource* dst, UINT sub, const D3D11_BOX* box, const void* data, UINT row, UINT depth)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) o->BeforeWrite(dst);
        original(ctx, dst, sub, box, data, row, depth);
    }
};
struct ClearRTHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        ID3D11RenderTargetView* view, const FLOAT* color)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) {
            ComPtr<ID3D11Resource> resource; if (view) view->GetResource(&resource);
            o->BeforeRead(resource.Get());
        }
        original(ctx, view, color);
    }
};
struct ClearDepthHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx,
        ID3D11DepthStencilView* view, UINT flags, FLOAT depth, UINT8 stencil)
    {
        if (auto* o = RDMRenderScope::Active(ctx)) {
            ComPtr<ID3D11Resource> r; if (view) view->GetResource(&r);
            if(flags&D3D11_CLEAR_DEPTH)o->BeforeWrite(r.Get());else o->BeforeRead(r.Get());
        }
        original(ctx, view, flags, depth, stencil);
        if ((flags & D3D11_CLEAR_DEPTH) != 0)
            if (const auto* observer = Observer(ctx); observer && observer->depthCleared)
                observer->depthCleared(observer->owner, ctx, view);
        RDMRenderScope::NotifyTargets(ctx);
    }
};
void ShaderChanged(ID3D11DeviceContext* ctx, bool resetState)
{
    if (auto* o = RDMRenderScope::Active(ctx)) {
        if (ocu_vrs_guard::CurrentReasons(ctx) & ocu_vrs_guard::CommandList) {
            o->BeforeCompute();o->InvalidateDepthGuide();
        }
        // The shared guard reports PS/blend/viewport setters with false and
        // full context reset/command-list restoration with true. A shader
        // change cannot change depth bindings, predication or stream output.
        o->StateChanged(ctx, resetState ? RDMRenderScope::GuideAll :
            RDMRenderScope::GuideShader | RDMRenderScope::GuideBlend);
    }
}
bool InitializeHooks()
{
    const auto status = MH_Initialize();
    return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}
constexpr std::array<UINT, 7> drawSlots{12, 13, 20, 21, 38, 39, 40};
using DrawEntries = std::array<void*, drawSlots.size()>;
DrawEntries CaptureDrawEntries(ID3D11DeviceContext* context)
{
    DrawEntries entries{};
    if (context) {
        const auto table = *reinterpret_cast<void***>(context);
        for (size_t i = 0; i < drawSlots.size(); ++i) entries[i] = table[drawSlots[i]];
    }
    return entries;
}
bool InstallDrawEntries(const DrawEntries& entries, const DrawEntries* initializedEntries = nullptr)
{
    if (!entries[0] || !InitializeHooks()) return false;
    const auto forwarding = [&](size_t i) { return initializedEntries && entries[i] != (*initializedEntries)[i]; };
    bool ok = true;
#define INSTALL(index, ...) ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, __VA_ARGS__), DrawHandler>::Install(entries[index], drawSlots[index], forwarding(index)) && ok
    INSTALL(0, UINT, UINT, INT);
    INSTALL(1, UINT, UINT);
    INSTALL(2, UINT, UINT, UINT, INT, UINT);
    INSTALL(3, UINT, UINT, UINT, UINT);
    ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*), DrawHandler>::Install(entries[4], drawSlots[4], forwarding(4)) && ok;
    INSTALL(5, ID3D11Buffer*, UINT);
    INSTALL(6, ID3D11Buffer*, UINT);
#undef INSTALL
    return ok;
}
struct NativeContextEntries {
    std::array<void*, 115> base{};
    std::array<void*, 134> extended{};
    bool hasContext1 = false;
};
NativeContextEntries CaptureContextEntries(ID3D11DeviceContext* context, ID3D11DeviceContext1* context1)
{
    NativeContextEntries entries;
    std::memcpy(entries.base.data(), *reinterpret_cast<void***>(context), sizeof(entries.base));
    if (context1) {
        std::memcpy(entries.extended.data(), *reinterpret_cast<void***>(context1), sizeof(entries.extended));
        entries.hasContext1 = true;
    }
    return entries;
}
struct NativeDevice {
    LUID adapter{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Multithread> multithread;
    BOOL protectedMode = FALSE;
    DrawEntries coldDrawEntries{}, warmDrawEntries{};
    NativeContextEntries coldEntries{}, warmEntries{};
};
NativeDevice* NativeContext(ID3D11Device* gameDevice, BOOL protectedMode)
{
    // Each logical adapter/device configuration needs both protection modes.
    static std::array<NativeDevice, 16> nativeDevices;
    static std::mutex nativeLock;
    std::lock_guard<std::mutex> lock(nativeLock);
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (!gameDevice || FAILED(gameDevice->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
        FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return nullptr;
    NativeDevice* native = nullptr;
    for (auto& d : nativeDevices) if (d.device && d.adapter.LowPart == desc.AdapterLuid.LowPart &&
        d.adapter.HighPart == desc.AdapterLuid.HighPart &&
        d.device->GetCreationFlags() == gameDevice->GetCreationFlags() &&
        d.device->GetFeatureLevel() == gameDevice->GetFeatureLevel() &&
        d.protectedMode == protectedMode && d.multithread &&
        d.multithread->GetMultithreadProtected() == protectedMode) { native = &d; break; }
    if (!native) for (auto& d : nativeDevices) if (!d.device) {
        NativeDevice candidate;
        const D3D_FEATURE_LEVEL levels[] = {gameDevice->GetFeatureLevel()};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, gameDevice->GetCreationFlags(),
            levels, 1, D3D11_SDK_VERSION, &candidate.device, nullptr, &candidate.context);
        if (FAILED(hr) && desc.VendorId == 0x1414)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, gameDevice->GetCreationFlags(),
                levels, 1, D3D11_SDK_VERSION, &candidate.device, nullptr, &candidate.context);
        if (FAILED(hr)) { ReportHookFailure("probe-device", 0, nullptr, hr); return nullptr; }
        hr = candidate.context->QueryInterface(IID_PPV_ARGS(&candidate.multithread));
        if (FAILED(hr)) { ReportHookFailure("probe-multithread", 0, nullptr, hr); return nullptr; }
        // Protection switches D3D11's context method entries. Probe the same
        // mode as the game without changing the game's threading contract.
        candidate.multithread->SetMultithreadProtected(protectedMode);
        if (candidate.multithread->GetMultithreadProtected() != protectedMode) {
            ReportHookFailure("probe-mode", 0, nullptr, protectedMode); return nullptr;
        }
        // A protected context initializes another set of native method entries
        // when normal context calls begin. Matching its protection flag alone
        // misses the normal draw path. Hook both snapshots before CSX captures
        // a native trampoline, and never warm or change the game's context.
        ComPtr<ID3D11DeviceContext1> context1;
        candidate.context->QueryInterface(IID_PPV_ARGS(&context1));
        candidate.coldEntries = CaptureContextEntries(candidate.context.Get(), context1.Get());
        candidate.coldDrawEntries = CaptureDrawEntries(candidate.context.Get());
        // A state getter initializes dispatch without sending an incomplete
        // pipeline to the driver, even for a nominally zero-vertex draw.
        ComPtr<ID3D11VertexShader> unusedShader;
        candidate.context->VSGetShader(&unusedShader, nullptr, nullptr);
        candidate.warmEntries = CaptureContextEntries(candidate.context.Get(), context1.Get());
        candidate.warmDrawEntries = CaptureDrawEntries(candidate.context.Get());
        // Changed cold entries forward through the selected context's draw
        // table. Run their native setup without starting an outer RDM scope.
        // Identical entries still own their draw normally.
        if (!InstallDrawEntries(candidate.coldDrawEntries, &candidate.warmDrawEntries)) return nullptr;
        if (!InstallDrawEntries(candidate.warmDrawEntries)) return nullptr;
        unsigned changedEntries = 0;
        for (size_t i = 0; i < drawSlots.size(); ++i)
            changedEntries += candidate.coldDrawEntries[i] != candidate.warmDrawEntries[i];
        OOVR_LOGF("RDM private probe draw hooks: multithreadProtected=%d coldInstalled=1 warmInstalled=1 changedEntries=%u freshForwarders=%u coldDrawIndexed=%p warmDrawIndexed=%p",
            protectedMode, changedEntries, changedEntries, candidate.coldDrawEntries[0], candidate.warmDrawEntries[0]);
        candidate.adapter = desc.AdapterLuid; candidate.protectedMode = protectedMode;
        d = std::move(candidate); native = &d; break;
    }
    if (!native) ReportHookFailure("probe-capacity", 0, nullptr, long(nativeDevices.size()));
    return native;
}
bool InstallDrawHooks(ID3D11DeviceContext* native)
{
    return InstallDrawEntries(CaptureDrawEntries(native));
}
bool Install(ID3D11DeviceContext* ctx)
{
    if (!ctx || !InitializeHooks()) return false;
    ComPtr<ID3D11Device> gameDevice; ctx->GetDevice(&gameDevice);
    ComPtr<ID3D11Multithread> gameMultithread;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&gameMultithread)))) return false;
    const BOOL protectedMode = gameMultithread->GetMultithreadProtected();
    auto* probe = NativeContext(gameDevice.Get(), protectedMode);
    if (!probe) return false;
    auto* native = probe->context.Get();
    bool ok = InstallDrawHooks(native);
    // Retain cold boundary/read hooks as well as the initialized variants.
    // D3D11 can update individual entries; the live table need not be all cold
    // or all initialized. Hook deduplication keeps repeated Arm calls bounded.
    for (const auto* snapshot : {&probe->coldEntries, &probe->warmEntries}) {
        const auto& v = snapshot->base;
#define INSTALL(slot, handler, ...) ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, __VA_ARGS__), handler>::Install(v[slot], slot) && ok
        if (v[33] != externalTargets.load()) {
            INSTALL(33, StateHandler<RDMRenderScope::GuideTargets | RDMRenderScope::GuideUavs>, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
        }
        if (v[34] != externalTargetsUav.load()) {
            INSTALL(34, StateHandler<RDMRenderScope::GuideTargets | RDMRenderScope::GuideUavs>, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
        }
        INSTALL(89, DepthBindingQueryHandler, UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**);
        INSTALL(90, DepthBindingQueryHandler, UINT, ID3D11RenderTargetView**, ID3D11DepthStencilView**, UINT, UINT, ID3D11UnorderedAccessView**);
        INSTALL(36, StateHandler<RDMRenderScope::GuideDepth>, ID3D11DepthStencilState*, UINT);
        INSTALL(43, StateHandler<RDMRenderScope::GuideNone>, ID3D11RasterizerState*);
        // VRSShaderGuard owns RSSetViewports and forwards it through ShaderChanged.
        // A second MinHook detour here prevents both RDM and shared VRS hooks arming.
        INSTALL(45, StateHandler<RDMRenderScope::GuideNone>, UINT, const D3D11_RECT*);
        INSTALL(30, StateHandler<RDMRenderScope::GuidePredicate>, ID3D11Predicate*, BOOL);
        INSTALL(37, StateHandler<RDMRenderScope::GuideStreamOutput>, UINT, ID3D11Buffer* const*, const UINT*);
        // Geometry coverage may only be reused with identical geometry inputs.
        INSTALL(11, GeometryStateHandler, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
        INSTALL(23, GeometryStateHandler, ID3D11GeometryShader*, ID3D11ClassInstance* const*, UINT);
        INSTALL(60, GeometryStateHandler, ID3D11HullShader*, ID3D11ClassInstance* const*, UINT);
        INSTALL(64, GeometryStateHandler, ID3D11DomainShader*, ID3D11ClassInstance* const*, UINT);
        INSTALL(17, GeometryStateHandler, ID3D11InputLayout*);
        INSTALL(18, GeometryStateHandler, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*);
        INSTALL(19, GeometryStateHandler, ID3D11Buffer*, DXGI_FORMAT, UINT);
        INSTALL(24, GeometryStateHandler, D3D11_PRIMITIVE_TOPOLOGY);
        for (UINT slot : {7u, 22u, 62u, 66u}) {
            INSTALL(slot, GeometryStateHandler, UINT, UINT, ID3D11Buffer* const*);
        }
        for (UINT slot : {26u, 32u, 61u, 65u}) {
            INSTALL(slot, GeometryStateHandler, UINT, UINT, ID3D11SamplerState* const*);
        }
        INSTALL(41, ComputeHandler, UINT, UINT, UINT);
        INSTALL(42, ComputeHandler, ID3D11Buffer*, UINT);
        // Distinct handler types are needed for equal signatures at different slots.
        struct PSRead : ReadHandler {}; struct VSRead : ReadHandler {}; struct GSRead : ReadHandler {};
        struct HSRead : ReadHandler {}; struct DSRead : ReadHandler {}; struct CSRead : ReadHandler {};
        INSTALL(8, PSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(25, VSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(31, GSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(59, HSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(63, DSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(67, CSRead, UINT, UINT, ID3D11ShaderResourceView* const*);
        INSTALL(47, CopyHandler, ID3D11Resource*, ID3D11Resource*);
        INSTALL(46, CopyRegionHandler, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
        INSTALL(57, ResolveHandler, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
        INSTALL(48, UpdateHandler, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
        INSTALL(50, ClearRTHandler, ID3D11RenderTargetView*, const FLOAT*);
        INSTALL(53, ClearDepthHandler, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
        INSTALL(54, ComputeHandler, ID3D11ShaderResourceView*);
        INSTALL(51, ComputeHandler, ID3D11UnorderedAccessView*, const UINT*);
        INSTALL(52, ComputeHandler, ID3D11UnorderedAccessView*, const FLOAT*);
        INSTALL(68, ComputeHandler, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
#undef INSTALL
        if (snapshot->hasContext1) {
            const auto& v1 = snapshot->extended;
#define INSTALL1(slot, ...) ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, __VA_ARGS__), ResourceWriteHandler>::Install(v1[slot], slot) && ok
            INSTALL1(115, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
            INSTALL1(116, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
            INSTALL1(117, ID3D11Resource*);
            INSTALL1(118, ID3D11View*);
            INSTALL1(132, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT);
            INSTALL1(133, ID3D11View*, const D3D11_RECT*, UINT);
#undef INSTALL1
            for (UINT slot : {119u, 120u, 121u, 122u})
                ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, UINT, UINT,
                    ID3D11Buffer* const*, const UINT*, const UINT*),
                    GeometryStateHandler>::Install(v1[slot], slot) && ok;
        }
        ok = Hook<HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP,
            UINT, D3D11_MAPPED_SUBRESOURCE*), MapHandler>::Install(v[14], 14) && ok;
    }
    static std::atomic<unsigned> reportedModes{0};
    const unsigned modeBit = protectedMode ? 2u : 1u;
    if (!(reportedModes.fetch_or(modeBit, std::memory_order_relaxed) & modeBit))
        OOVR_LOGF("RDM context hooks: multithreadProtected=%d nativeModeMatched=1 installed=%d", protectedMode, ok);
    return ok;
}
bool SupportedColor(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_R32G32B32A32_FLOAT: return true;
    default: return false;
    }
}
}

struct RDMRenderScope::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> expectedDepth, submitted, privateDepth, depthOwner, coverage;
    ComPtr<ID3D11DepthStencilView> privateDSV;
    ComPtr<ID3D11ShaderResourceView> depthSRV, coverageSRV;
    ComPtr<ID3D11RenderTargetView> coverageRTV;
    ComPtr<ID3D11Texture2D> eligibility;
    ComPtr<ID3D11ShaderResourceView> eligibilitySRV;
    ComPtr<ID3D11UnorderedAccessView> eligibilityUAV;
    ComPtr<ID3D11ComputeShader> eligibilityCS;
    ComPtr<ID3D11Buffer> eligibilityCB;
    DensityMaskManager resolver;
    ocu_rdm::GpuTiming gpuTiming{1};
    std::uint64_t deliveredGpuSample = 0;
    ocu_vrs_scope::SceneScope scope;
    DensityMaskManager::EyeRegion regions[2];
    DensityMaskManager::PatternSettings pattern;
    float centers[4]{};
    unsigned width = 0, height = 0;
    bool armed = false, busy = false, dirty = true, eligible = false, pending = false;
    bool privateDepthBound = false;
    bool preparedMask = false;
    bool colorCoverageReady = false, colorCoverageReusable = false;
    std::array<std::uint64_t, 8> colorDrawKey{};
    struct RasterGeometry {
        D3D11_RASTERIZER_DESC raster{};
        std::array<D3D11_VIEWPORT, 16> viewports{};
        std::array<D3D11_RECT, 16> scissors{};
        UINT viewportCount = 0, scissorCount = 0;
    } currentRasterGeometry{}, capturedRasterGeometry{};
    ComPtr<ID3D11Texture2D> capturedCoverageDepth;
    bool invalidatingGuide = false;
    Statistics stats;
    DiagnosticSnapshot diagnostics;
    unsigned scopeProbeOrdinal = 0, noPixelShaderProbeOrdinal = 0;
    std::array<std::uint64_t, 5> queryProbeKeys{};
    unsigned queryProbeKeyCount = 0;
    QueryReject lastQueryReject = QueryReject::Count;
    struct Targets {
        std::array<ComPtr<ID3D11RenderTargetView>, 8> views;
        std::array<ComPtr<ID3D11Texture2D>, 8> colors;
        std::array<UINT8, 8> writeMasks{};
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11Texture2D> depth;
        UINT count = 0, eyes = 0;
        bool partialWrites = false, inactiveTargets = false;
    } current, batch;
    struct GuideState {
        ComPtr<ID3D11DepthStencilView> view;
        bool dirty = true, candidate = false, invalidate = false, clear = false;
        unsigned changed = GuideAll;
        D3D11_DEPTH_STENCIL_DESC depth{};
        bool expectedBound = false, readOnlyDepth = false;
        bool predicate = false, streamOutput = false, unorderedAccess = false;
        UINT sampleMask = 0;
    } guideState;
    struct Busy {
        Impl& p; bool was;
        Busy(Impl& p) : p(p), was(p.busy) { p.busy = true; }
        ~Busy() { p.busy = was; }
    };
    struct CpuTimer {
        double* destination;
        std::chrono::steady_clock::time_point start{};
        CpuTimer(bool enabled, double& value) : destination(enabled ? &value : nullptr)
        { if (destination) start = std::chrono::steady_clock::now(); }
        ~CpuTimer()
        { if (destination) *destination += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
    };
    void Bind(ID3D11DepthStencilView* depth)
    {
        ID3D11RenderTargetView* views[8]{};
        for (UINT i = 0; i < current.count; ++i) views[i] = current.views[i].Get();
        context->OMSetRenderTargets(current.count, views, depth);
    }
    void RestoreOriginalDepth()
    {
        if (!privateDepthBound) return;
        Busy guard(*this);
        // Query can replace current before a pending batch is resolved. The
        // retained binding always belongs to batch, which owns its originals.
        ID3D11RenderTargetView* views[8]{};
        for (UINT i = 0; i < batch.count; ++i) views[i] = batch.views[i].Get();
        privateDepthBound = false;
        context->OMSetRenderTargets(batch.count, views, batch.dsv.Get());
        ++stats.originalDepthRestores;
    }
    static bool Probe(unsigned ordinal)
    {
        // At most four descriptor probes per category/Arm. Later probes give
        // scene color a chance to replace an early shadow/depth-only sample.
        return ordinal == 1 || ordinal == 16 || ordinal == 256 || ordinal == 4096;
    }
    bool ProbeQuery(QueryReject reason)
    {
        const unsigned cached = ocu_vrs_guard::CurrentReasons(context.Get());
        const std::uint64_t key = (std::uint64_t(static_cast<unsigned>(reason)) << 32) | cached;
        bool newKey = true;
        for (unsigned i = 0; i < queryProbeKeyCount; ++i)
            if (queryProbeKeys[i] == key) { newKey = false; break; }
        bool capture = false;
        if (newKey && queryProbeKeyCount < queryProbeKeys.size()) {
            queryProbeKeys[queryProbeKeyCount++] = key;
            capture = true;
        }
        // A null-depth PS must not consume the first distinct color rejection.
        // Also reserve bounded repeat probes for a cache stuck at NoPixelShader:
        // actual non-null color can share that same stale cached reason.
        if (cached & ocu_vrs_guard::NoPixelShader) {
            ++noPixelShaderProbeOrdinal;
            capture = capture || noPixelShaderProbeOrdinal == 2 || noPixelShaderProbeOrdinal == 256;
        }
        return capture;
    }
    void CaptureDiagnostic(RejectedStateDiagnostic& destination, QueryReject reason,
        StartFailure failure, bool scopeMatched)
    {
        RejectedStateDiagnostic sample;
        sample.valid = true; sample.scopeMatched = scopeMatched;
        sample.queryReject = reason; sample.startFailure = failure;
        sample.queryOrdinal = stats.stateQueries; sample.drawOrdinal = stats.draws;
        sample.expectedWidth = width; sample.expectedHeight = height;
        sample.cachedReasons = ocu_vrs_guard::CurrentReasons(context.Get());
        sample.watchedContext = ocu_vrs_guard::CurrentViewports(context.Get()) != nullptr;
        ComPtr<ID3D11PixelShader> shader;
        ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
        UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
        context->PSGetShader(&shader, instances, &instanceCount);
        sample.hasPixelShader = shader != nullptr;
        sample.classInstances = shader ? instanceCount : 0;
        sample.actualShaderReasons = ocu_vrs_guard::ShaderReasons(shader.Get()) |
            (sample.classInstances ? ocu_vrs_guard::ClassLinkage : ocu_vrs_guard::Compatible);
        sample.actualColorOutputs = ocu_vrs_guard::ShaderColorOutputs(shader.Get());
        for (auto* instance : instances) if (instance) instance->Release();
        // CurrentReasons includes context blend and command-list bits. Compare
        // shader bits separately; neither context flag is missing PS metadata.
        constexpr unsigned contextBits = ocu_vrs_guard::AlphaToCoverage | ocu_vrs_guard::CommandList;
        sample.shaderCacheMismatch =
            (sample.cachedReasons & ~contextBits) != (sample.actualShaderReasons & ~contextBits);
        ComPtr<ID3D11BlendState> blend;
        context->OMGetBlendState(&blend, nullptr, &sample.sampleMask);
        D3D11_BLEND_DESC blendDesc{}; if (blend) blend->GetDesc(&blendDesc);
        D3D11_BLEND_DESC1 blendDesc1{};
        ComPtr<ID3D11BlendState1> blend1;
        if (blend && SUCCEEDED(blend.As(&blend1))) blend1->GetDesc1(&blendDesc1);
        if (!blend) {
            for (auto& target : blendDesc.RenderTarget) {
                target.SrcBlend = D3D11_BLEND_ONE; target.DestBlend = D3D11_BLEND_ZERO;
                target.BlendOp = D3D11_BLEND_OP_ADD; target.SrcBlendAlpha = D3D11_BLEND_ONE;
                target.DestBlendAlpha = D3D11_BLEND_ZERO; target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
                target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            }
        }
        sample.actualAlphaToCoverage = blendDesc.AlphaToCoverageEnable != FALSE;
        sample.blendCacheMismatch =
            bool(sample.cachedReasons & ocu_vrs_guard::AlphaToCoverage) != sample.actualAlphaToCoverage;
        ComPtr<ID3D11DepthStencilState> depthState;
        context->OMGetDepthStencilState(&depthState, &sample.stencilReference);
        sample.hasDepthState = depthState != nullptr;
        if (depthState) depthState->GetDesc(&sample.depthState);
        if (current.dsv) {
            current.dsv->GetDesc(&sample.depthView);
            ComPtr<ID3D11Resource> resource; current.dsv->GetResource(&resource);
            sample.expectedDepthBound = expectedDepth && resource.Get() == expectedDepth.Get();
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                sample.hasDepthTexture = true; texture->GetDesc(&sample.depthTexture);
            }
        }
        for (unsigned i = 0; i < current.count; ++i) if (current.views[i]) {
            auto& target = sample.targets[i]; target.bound = true;
            target.effectiveBlend = blendDesc.RenderTarget[blendDesc.IndependentBlendEnable ? i : 0];
            target.logicOp = blendDesc1.RenderTarget[blendDesc1.IndependentBlendEnable ? i : 0].LogicOpEnable != FALSE;
            sample.boundRtvMask |= 1u << i;
            current.views[i]->GetDesc(&target.view);
            ComPtr<ID3D11Resource> resource; current.views[i]->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource.As(&texture))) {
                target.texture = true; texture->GetDesc(&target.description);
            }
        }
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetViewports(&count, viewports); sample.viewportCount = count;
        if (count) sample.firstViewport = viewports[0];
        ComPtr<ID3D11RasterizerState> rasterizer; context->RSGetState(&rasterizer);
        sample.hasRasterizer = rasterizer != nullptr;
        if (rasterizer) rasterizer->GetDesc(&sample.rasterizer);
        if (sample.rasterizer.ScissorEnable) {
            D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
            context->RSGetScissorRects(&count, scissors); sample.scissorCount = count;
            if (count) sample.firstScissor = scissors[0];
        }
        ++stats.diagnosticSamples;
        if (sample.shaderCacheMismatch) ++stats.shaderCacheMismatchSamples;
        if (sample.blendCacheMismatch) ++stats.blendCacheMismatchSamples;
        sample.selectionScore = (scopeMatched ? 8u : 0u) +
            (sample.expectedDepthBound ? 4u : 0u) + (sample.hasPixelShader ? 2u : 0u) +
            (sample.boundRtvMask ? 1u : 0u);
        if (reason != QueryReject::Count) {
            auto& category = diagnostics.byReason[static_cast<unsigned>(reason)];
            if (!category.valid || sample.selectionScore >= category.selectionScore)
                category = sample;
        }
        if (!destination.valid || sample.selectionScore >= destination.selectionScore)
            destination = sample;
    }
    bool Reject(QueryReject reason)
    {
        lastQueryReject = reason;
        ++stats.queryRejectCounts[static_cast<unsigned>(reason)];
        if (diagnostics.enabled && current.count) {
            const bool scene = reason != QueryReject::SceneScope;
            const bool capture = scene ? ProbeQuery(reason) : Probe(++scopeProbeOrdinal);
            if (capture) CaptureDiagnostic(scene ? diagnostics.query : diagnostics.scope,
                reason, StartFailure::Count, scene);
        }
        return false;
    }
    bool FailStart(StartFailure stage, HRESULT hr = S_OK, bool known = false)
    {
        ++stats.startFailureCounts[static_cast<unsigned>(stage)];
        stats.lastStartFailure = stage; stats.lastStartHresult = hr;
        stats.lastStartHresultKnown = known;
        return false;
    }
    void QueryGuide()
    {
        CpuTimer timer(diagnostics.enabled, stats.guideQueryCpuMs);
        ++stats.guideStateQueries;
        auto& g = guideState;
        g.dirty = false; g.candidate = false; g.clear = false;
        auto refresh = [&](unsigned flag) {
            const bool changed = (g.changed & flag) != 0;
            g.changed &= ~flag;
            return changed;
        };
        if (refresh(GuideDepth)) {
            ++stats.guideDepthReads;
            ComPtr<ID3D11DepthStencilState> state; context->OMGetDepthStencilState(&state, nullptr);
            g.depth = {};
            if (state) state->GetDesc(&g.depth);
            else { g.depth.DepthEnable = TRUE; g.depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; g.depth.DepthFunc = D3D11_COMPARISON_LESS; }
        }
        const auto& ds = g.depth;
        if (!ds.DepthEnable || ds.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ALL ||
            ds.DepthFunc == D3D11_COMPARISON_EQUAL || ds.DepthFunc == D3D11_COMPARISON_NEVER) return;
        if (refresh(GuideTargets)) {
            ++stats.guideTargetReads;
            g.view.Reset(); context->OMGetRenderTargets(0, nullptr, &g.view);
            ComPtr<ID3D11Resource> resource; if (g.view) g.view->GetResource(&resource);
            g.expectedBound = resource && resource.Get() == expectedDepth.Get();
            D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; if (g.view) g.view->GetDesc(&dv);
            g.readOnlyDepth = (dv.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0;
        }
        if (!g.expectedBound) return;
        const auto reasons = ocu_vrs_guard::CurrentReasons(context.Get());
        g.changed &= ~GuideShader;
        g.candidate = true; g.invalidate = (reasons & ocu_vrs_guard::NoPixelShader) == 0;
        if (refresh(GuidePredicate)) {
            ++stats.guideHazardReads;
            ComPtr<ID3D11Predicate> predicate; context->GetPredication(&predicate, nullptr);
            g.predicate = predicate != nullptr;
        }
        if (refresh(GuideBlend)) {
            ++stats.guideHazardReads;
            context->OMGetBlendState(nullptr, nullptr, &g.sampleMask);
        }
        if (refresh(GuideStreamOutput)) {
            ++stats.guideHazardReads;
            ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{}; context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, streams);
            g.streamOutput = false;
            for (auto* stream : streams) if (stream) { g.streamOutput = true; stream->Release(); }
        }
        if (refresh(GuideUavs)) {
            ++stats.guideHazardReads;
            ID3D11UnorderedAccessView* uavs[64]{};
            const UINT uavCount = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavCount, uavs);
            g.unorderedAccess = false;
            for (auto* uav : uavs) if (uav) { g.unorderedAccess = true; uav->Release(); }
        }
        constexpr unsigned forbidden = ocu_vrs_guard::Unclassified | ocu_vrs_guard::DepthOrCoverage |
            ocu_vrs_guard::UnorderedAccess | ocu_vrs_guard::ClassLinkage | ocu_vrs_guard::CommandList;
        g.clear = g.predicate || g.streamOutput || g.unorderedAccess ||
            (reasons & ocu_vrs_guard::AlphaToCoverage) || g.sampleMask != 0xffffffffu ||
            g.readOnlyDepth || (reasons & forbidden);
    }
    bool Query()
    {
        ++stats.stateQueries;
        // OM getters expose the logical game DSV while compatible draws keep
        // the physical private binding. Revalidation itself need not rebind.
        current = {}; dirty = false;
        ID3D11RenderTargetView* views[8]{};
        context->OMGetRenderTargets(8, views, &current.dsv);
        for (unsigned i = 0; i < 8; ++i) { current.views[i].Attach(views[i]); if (views[i]) current.count = i + 1; }
        if (!scope.Matches(context.Get(), 8, views, current.dsv.Get())) return Reject(QueryReject::SceneScope);
        const auto reasons = ocu_vrs_guard::CurrentReasons(context.Get());
        stats.cachedShaderReasonUnion |= reasons;
        if (reasons != ocu_vrs_guard::Compatible) return Reject(QueryReject::ShaderReasons);
        if (!resolver.HasDepthGuideDraws()) { ++stats.noGuideStates; return Reject(QueryReject::NoDepthGuide); }
        ComPtr<ID3D11DepthStencilState> state;
        context->OMGetDepthStencilState(&state, nullptr);
        if (!state) return Reject(QueryReject::NoDepthState);
        D3D11_DEPTH_STENCIL_DESC depth{}; state->GetDesc(&depth);
        // Equality cannot create new depth, even with depth writes enabled.
        // Blending and stencil-dependent coverage require full-rate rendering.
        if (!depth.DepthEnable) return Reject(QueryReject::DepthDisabled);
        if (depth.DepthFunc != D3D11_COMPARISON_EQUAL) return Reject(QueryReject::DepthComparison);
        if (depth.StencilEnable) return Reject(QueryReject::Stencil);
        ComPtr<ID3D11BlendState> blend; UINT sampleMask = 0;
        context->OMGetBlendState(&blend, nullptr, &sampleMask);
        if (sampleMask != 0xffffffffu) return Reject(QueryReject::SampleMask);
        D3D11_BLEND_DESC blendDesc{};
        if (blend) blend->GetDesc(&blendDesc);
        bool anyColorWrite = false;
        for (unsigned i = 0; i < current.count; ++i) if (views[i]) {
            current.writeMasks[i] = blend ?
                blendDesc.RenderTarget[blendDesc.IndependentBlendEnable ? i : 0].RenderTargetWriteMask :
                D3D11_COLOR_WRITE_ENABLE_ALL;
            anyColorWrite = anyColorWrite || current.writeMasks[i] != 0;
            current.partialWrites = current.partialWrites ||
                (current.writeMasks[i] != 0 && current.writeMasks[i] != D3D11_COLOR_WRITE_ENABLE_ALL);
            current.inactiveTargets = current.inactiveTargets || current.writeMasks[i] == 0;
        }
        // Bound MRTs can be intentionally read-only for this draw. Only
        // shader outputs and channels that the draw can write need resolving.
        if (!anyColorWrite) return Reject(QueryReject::WriteMask);
        ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps, nullptr, nullptr);
        const auto outputs = ocu_vrs_guard::ShaderColorOutputs(ps.Get());
        for (unsigned i = 0; i < current.count; ++i)
            if (current.writeMasks[i] && !(outputs & (1u << i))) return Reject(QueryReject::ShaderOutputs);
        ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{};
        context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, streams);
        bool hasStream = false;
        for (auto* stream : streams) if (stream) { hasStream = true; stream->Release(); }
        if (hasStream) return Reject(QueryReject::StreamOutput);
        if (blend) {
            const auto& b = blendDesc;
            if (b.AlphaToCoverageEnable) return Reject(QueryReject::AlphaToCoverage);
            ComPtr<ID3D11BlendState1> blend1;
            if (SUCCEEDED(blend.As(&blend1))) {
                D3D11_BLEND_DESC1 b1{}; blend1->GetDesc1(&b1);
                for (unsigned i = 0; i < current.count; ++i)
                    if (current.writeMasks[i] && b1.RenderTarget[b1.IndependentBlendEnable ? i : 0].LogicOpEnable) return Reject(QueryReject::LogicOp);
            }
            for (unsigned i = 0; i < current.count; ++i) if (current.writeMasks[i]) {
                const auto& target = b.RenderTarget[b.IndependentBlendEnable ? i : 0];
                if (target.BlendEnable) return Reject(QueryReject::Blend);
            }
        }
        ComPtr<ID3D11Predicate> predicate; context->GetPredication(&predicate, nullptr);
        if (predicate) return Reject(QueryReject::Predicate);
        ComPtr<ID3D11Resource> resource; current.dsv->GetResource(&resource);
        if (FAILED(resource.As(&current.depth))) return Reject(QueryReject::DepthResource);
        D3D11_TEXTURE2D_DESC dd{}; current.depth->GetDesc(&dd);
        if (!(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || dd.MipLevels != 1) return Reject(QueryReject::DepthTexture);
        for (unsigned i = 0; i < current.count; ++i) if (current.writeMasks[i]) {
            resource.Reset(); views[i]->GetResource(&resource);
            if (FAILED(resource.As(&current.colors[i]))) return Reject(QueryReject::ColorResource);
            D3D11_TEXTURE2D_DESC td{}; current.colors[i]->GetDesc(&td);
            if (td.MipLevels != 1 || !SupportedColor(td.Format)) return Reject(QueryReject::ColorTexture);
        }
        // An OM UAV is an observable side effect even when the current shader
        // does not declare it. Do not disturb bindings or hidden UAV counters.
        const UINT slots = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        ID3D11UnorderedAccessView* uavs[64]{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            current.count, slots-current.count, uavs+current.count);
        bool hasUav = false;
        for (auto* uav : uavs) if (uav) { hasUav = true; uav->Release(); }
        if (hasUav) return Reject(QueryReject::OmUav);
        D3D11_VIEWPORT vp[16]{}; UINT count = 16;
        context->RSGetViewports(&count, vp);
        ComPtr<ID3D11RasterizerState> rs; context->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{}; if (rs) rs->GetDesc(&rd);
        if (rs && (rd.FillMode != D3D11_FILL_SOLID || rd.DepthBias || rd.SlopeScaledDepthBias != 0 || !rd.DepthClipEnable)) return Reject(QueryReject::Rasterizer);
        D3D11_RECT scissors[16]{}; UINT sc = 16;
        if (rd.ScissorEnable) context->RSGetScissorRects(&sc, scissors);
        for (int eye = 0; eye < 2; ++eye) {
            const auto& e = regions[eye];
            for (UINT v = 0; v < count; ++v) {
                // The guide records SV_Position.z after the game's viewport
                // transform. Its stored depth remains valid for normalized
                // subranges; do not require Skyrim's far bound to equal 1.
                if (!std::isfinite(vp[v].MinDepth) || !std::isfinite(vp[v].MaxDepth) ||
                    vp[v].MinDepth < 0 || vp[v].MaxDepth > 1 ||
                    vp[v].MinDepth >= vp[v].MaxDepth) continue;
                bool covers = vp[v].TopLeftX <= e.left && vp[v].TopLeftY <= e.top &&
                    vp[v].TopLeftX+vp[v].Width >= e.left+e.width && vp[v].TopLeftY+vp[v].Height >= e.top+e.height;
                if (rd.ScissorEnable) covers = covers && v < sc && scissors[v].left <= e.left &&
                    scissors[v].top <= e.top && scissors[v].right >= e.left+e.width && scissors[v].bottom >= e.top+e.height;
                if (covers) current.eyes |= 1u << eye;
            }
        }
        if (!current.eyes) return Reject(QueryReject::Viewport);
        currentRasterGeometry = {};
        currentRasterGeometry.raster = rd;
        currentRasterGeometry.viewportCount = count;
        std::copy_n(vp, count, currentRasterGeometry.viewports.begin());
        if (rd.ScissorEnable) {
            currentRasterGeometry.scissorCount = sc;
            std::copy_n(scissors, sc, currentRasterGeometry.scissors.begin());
        }
        if (colorCoverageReady && (capturedCoverageDepth != current.depth ||
            std::memcmp(&capturedRasterGeometry, &currentRasterGeometry, sizeof(RasterGeometry))))
            colorCoverageReady = false;
        lastQueryReject = QueryReject::Count;
        ++stats.queryAccepted;
        return true;
    }
    bool SameBatch() const
    {
        if (batch.depth != current.depth || batch.eyes != current.eyes || batch.count != current.count) return false;
        if (batch.writeMasks != current.writeMasks) return false;
        for (unsigned i = 0; i < 8; ++i) if (batch.views[i] != current.views[i]) return false;
        return true;
    }
    bool PrepareDepth()
    {
        if (depthOwner == current.depth && privateDepth && depthSRV && eligibilityCB && eligibilityCS) return true;
        preparedMask = false;
        depthOwner.Reset(); privateDepth.Reset(); privateDSV.Reset(); depthSRV.Reset();
        coverage.Reset(); coverageRTV.Reset(); coverageSRV.Reset();
        D3D11_TEXTURE2D_DESC d{}; current.depth->GetDesc(&d);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; current.dsv->GetDesc(&dv);
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        switch (dv.Format) {
        case DXGI_FORMAT_D16_UNORM: sv.Format = DXGI_FORMAT_R16_UNORM; break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
        case DXGI_FORMAT_D32_FLOAT: sv.Format = DXGI_FORMAT_R32_FLOAT; break;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: sv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
        default: return FailStart(StartFailure::DepthFormat);
        }
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        auto created = [&](HRESULT hr, StartFailure stage) {
            if (FAILED(hr)) return FailStart(stage, hr, true);
            return true;
        };
        if (!created(device->CreateShaderResourceView(current.depth.Get(), &sv, &depthSRV), StartFailure::DepthSrv)) return false;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL; d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; dv.Flags = 0;
        if (!created(device->CreateTexture2D(&d, nullptr, &privateDepth), StartFailure::PrivateDepthTexture) ||
            !created(device->CreateDepthStencilView(privateDepth.Get(), &dv, &privateDSV), StartFailure::PrivateDsv)) return false;
        d.Format = DXGI_FORMAT_R8_UNORM; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!created(device->CreateTexture2D(&d, nullptr, &coverage), StartFailure::CoverageTexture) ||
            !created(device->CreateRenderTargetView(coverage.Get(), nullptr, &coverageRTV), StartFailure::CoverageRtv) ||
            !created(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageSRV), StartFailure::CoverageSrv)) return false;
        eligibility.Reset(); eligibilitySRV.Reset(); eligibilityUAV.Reset();
        d.Width = (regions[0].width+7)/8 + (regions[1].width+7)/8;
        d.Height = (std::max(regions[0].height, regions[1].height)+7)/8;
        d.Format = DXGI_FORMAT_R32_FLOAT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (!created(device->CreateTexture2D(&d, nullptr, &eligibility), StartFailure::EligibilityTexture) ||
            !created(device->CreateShaderResourceView(eligibility.Get(), nullptr, &eligibilitySRV), StartFailure::EligibilitySrv) ||
            !created(device->CreateUnorderedAccessView(eligibility.Get(), nullptr, &eligibilityUAV), StartFailure::EligibilityUav)) return false;
        if (!eligibilityCS || !eligibilityCB) {
            eligibilityCS.Reset(); eligibilityCB.Reset();
            ComPtr<ID3DBlob> code, errors;
            HRESULT hr = D3DCompile(depthEligibilityShader, sizeof(depthEligibilityShader)-1,
                "OCU RDM depth eligibility", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (!created(hr, StartFailure::EligibilityCompile) ||
                !created(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &eligibilityCS), StartFailure::EligibilityShader)) return false;
            D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.Usage = D3D11_USAGE_DYNAMIC; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (!created(device->CreateBuffer(&cb, nullptr, &eligibilityCB), StartFailure::EligibilityBuffer)) return false;
            Name(eligibilityCS.Get(), "OCU RDM depth eligibility CS"); Name(eligibilityCB.Get(), "OCU RDM eligibility constants");
        }
        Name(eligibility.Get(), "OCU RDM depth-continuous clusters");
        Name(eligibilitySRV.Get(), "OCU RDM eligibility SRV"); Name(eligibilityUAV.Get(), "OCU RDM eligibility UAV");
        Name(privateDepth.Get(), "OCU RDM private depth"); Name(privateDSV.Get(), "OCU RDM private DSV");
        Name(depthSRV.Get(), "OCU RDM unmodified scene depth SRV");
        Name(coverage.Get(), "OCU RDM skipped-pixel coverage");
        Name(coverageRTV.Get(), "OCU RDM coverage RTV"); Name(coverageSRV.Get(), "OCU RDM coverage SRV");
        depthOwner = current.depth;
        return true;
    }
    bool BuildEligibility()
    {
        ComPtr<ID3D11ComputeShader> shader; ID3D11ClassInstance* instances[D3D11_SHADER_MAX_INTERFACES]{};
        UINT instanceCount = D3D11_SHADER_MAX_INTERFACES;
        context->CSGetShader(&shader, instances, &instanceCount);
        ComPtr<ID3D11DeviceContext1> ctx1; context.As(&ctx1);
        UINT first = 0, count = 0;
        ComPtr<ID3D11Buffer> cb;
        if (ctx1) ctx1->CSGetConstantBuffers1(0, 1, &cb, &first, &count);
        else context->CSGetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* savedSRVs[2]{}; context->CSGetShaderResources(0, 2, savedSRVs);
        ComPtr<ID3D11UnorderedAccessView> uav; context->CSGetUnorderedAccessViews(0, 1, &uav);
        // The original DSV is unbound while its clean depth is sampled.
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CSSetShader(eligibilityCS.Get(), nullptr, 0);
        ID3D11Buffer* input = eligibilityCB.Get(); context->CSSetConstantBuffers(0, 1, &input);
        ID3D11ShaderResourceView* inputs[]={depthSRV.Get(),resolver.DepthGuide()};context->CSSetShaderResources(0,2,inputs);
        auto* out = eligibilityUAV.Get(); UINT keep = 0xffffffffu;
        context->CSSetUnorderedAccessViews(0, 1, &out, &keep);
        bool ok = true;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& r = regions[eye];
            UINT constants[8] = {UINT(r.left), UINT(r.top), UINT(r.width), UINT(r.height),
                eye ? UINT((regions[0].width+7)/8) : 0u, 0, 0, 0};
            D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};current.dsv->GetDesc(&depthView);
            const float tolerance=depthView.Format==DXGI_FORMAT_D16_UNORM?1.f/65535.f:1.e-7f;
            std::memcpy(constants+5,&tolerance,sizeof(tolerance));
            D3D11_MAPPED_SUBRESOURCE map{};
            const HRESULT mapResult = context->Map(input, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
            if (FAILED(mapResult)) { ok = FailStart(StartFailure::EligibilityMap, mapResult, true); break; }
            std::memcpy(map.pData, constants, sizeof(constants)); context->Unmap(input, 0);
            context->Dispatch((r.width+7)/8, (r.height+7)/8, 1);
        }
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        context->CSSetShaderResources(0,2,savedSRVs);
        for(auto* srv:savedSRVs)if(srv)srv->Release();
        auto* oldUav = uav.Get(); context->CSSetUnorderedAccessViews(0, 1, &oldUav, &keep);
        auto* oldCB = cb.Get();
        if (ctx1) ctx1->CSSetConstantBuffers1(0, 1, &oldCB, &first, &count);
        else context->CSSetConstantBuffers(0, 1, &oldCB);
        context->CSSetShader(shader.Get(), instances, instanceCount);
        for (UINT i = 0; i < instanceCount; ++i) instances[i]->Release();
        Bind(current.dsv.Get());
        return ok;
    }
    bool Start()
    {
        ++stats.startAttempts;
        Busy guard(*this);
        if (!PrepareDepth()) return false;
        if (!resolver.Initialize(device.Get())) return FailStart(StartFailure::ResolverInitialize);
        resolver.SetPatternSettings(pattern);
        resolver.SetProjectionCenters(centers[0], centers[1], centers[2], centers[3]);
        ID3D11Texture2D* colors[8]{};
        for (unsigned i=0;i<current.count;++i)colors[i]=current.colors[i].Get();
        if (!resolver.PrepareMRTTargets(colors,current.count,width,height,regions[0],regions[1],current.writeMasks.data()))
            return FailStart(StartFailure::ResolverTargets);
        // Equality-only color draws preserve depth. Reuse its mask until a
        // depth/ownership write or frame change invalidates the preparation.
        if (!preparedMask) {
            struct TimedPreparation {
                ocu_rdm::GpuTiming& timing;
                explicit TimedPreparation(ocu_rdm::GpuTiming& value) : timing(value)
                { timing.Begin(ocu_rdm::GpuTiming::Stage::Preparation); }
                ~TimedPreparation() { timing.End(ocu_rdm::GpuTiming::Stage::Preparation); }
            } timed(gpuTiming);
            context->CopyResource(privateDepth.Get(), current.depth.Get());
            if (!BuildEligibility()) return false;
            const float zero[4]{}; context->ClearRenderTargetView(coverageRTV.Get(), zero);
            if (!resolver.ApplyDepthMask(privateDSV.Get(), 1.0f, coverageRTV.Get(), eligibilitySRV.Get()))
                return FailStart(StartFailure::ApplyMask);
            preparedMask = true;
            ++stats.maskPreparations;
        } else ++stats.maskReuses;
        batch = current; pending = true; ++stats.batches;
        return true;
    }
    void Finish()
    {
        // The resolver saves/restores OM state. Never let it capture our
        // private depth, including failures and frame/shutdown boundaries.
        RestoreOriginalDepth();
        if (!pending) return;
        Busy guard(*this);
        pending = false;
        gpuTiming.Begin(ocu_rdm::GpuTiming::Stage::Resolve);
        const bool ok = resolver.ResolveMRTs(coverageSRV.Get(), batch.eyes);
        gpuTiming.End(ocu_rdm::GpuTiming::Stage::Resolve);
        for (unsigned i=0;i<batch.count;++i)if(batch.colors[i])++stats.resolves;
        if (!ok) {
            armed = false;
            OOVR_LOG("RDM handoff: GPU color resolve failed; inspect device status");
        }
        batch = {};
    }
};

const char* RDMRenderScope::QueryRejectName(QueryReject reason) noexcept
{
    static constexpr const char* names[] = {
        "scene-scope", "shader-reasons", "no-depth-guide", "no-depth-state", "depth-disabled",
        "depth-comparison", "stencil", "sample-mask", "shader-outputs", "stream-output",
        "alpha-to-coverage", "logic-op", "blend", "write-mask", "predicate", "depth-resource",
        "depth-texture", "color-resource", "color-texture", "om-uav", "rasterizer", "viewport"
    };
    static_assert(sizeof(names) / sizeof(*names) == QueryRejectCount);
    const auto index = static_cast<unsigned>(reason);
    return index < QueryRejectCount ? names[index] : "none";
}
const char* RDMRenderScope::StartFailureName(StartFailure stage) noexcept
{
    static constexpr const char* names[] = {
        "depth-format", "depth-srv", "private-depth-texture", "private-dsv",
        "coverage-texture", "coverage-rtv", "coverage-srv", "eligibility-texture",
        "eligibility-srv", "eligibility-uav", "eligibility-compile", "eligibility-shader",
        "eligibility-buffer", "resolver-initialize", "resolver-targets", "eligibility-map", "apply-mask"
    };
    static_assert(sizeof(names) / sizeof(*names) == StartFailureCount);
    const auto index = static_cast<unsigned>(stage);
    return index < StartFailureCount ? names[index] : "none";
}

RDMRenderScope::RDMRenderScope() : impl(std::make_unique<Impl>()) {}
RDMRenderScope::~RDMRenderScope() { EndFrame(); }
RDMRenderScope* RDMRenderScope::Active(ID3D11DeviceContext* ctx)
{
    if (ctx != activeContext.load(std::memory_order_acquire)) return nullptr;
    auto* owner = active.load(std::memory_order_acquire);
    return owner && owner->impl->context.Get() == ctx && owner->impl->armed && !owner->impl->busy ? owner : nullptr;
}
bool RDMRenderScope::PrepareDrawHooks(ID3D11Device* device)
{
    if (!device) return false;
    // Install before a shader mod captures its native-call trampoline. Its
    // accepted-draw callbacks must run outside RDM's busy scope so their state
    // queries expose the original DSV and output setters restore it physically.
    // Only draw methods belong here; compositor/state hooks are installed later.
    const bool unprotected = NativeContext(device, FALSE) != nullptr;
    const bool protectedMode = NativeContext(device, TRUE) != nullptr;
    return unprotected && protectedMode;
}
bool RDMRenderScope::Arm(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth,
    ID3D11Texture2D* submitted, int width, int height,
    const DensityMaskManager::EyeRegion& left, const DensityMaskManager::EyeRegion& right,
    const DensityMaskManager::PatternSettings& pattern, const float centers[4], bool diagnosticsEnabled)
{
    EndFrame();
    impl->stats = {}; impl->diagnostics = {};
    impl->preparedMask = false;
    impl->colorCoverageReady = false;
    impl->scopeProbeOrdinal = impl->noPixelShaderProbeOrdinal = 0;
    impl->queryProbeKeys = {}; impl->queryProbeKeyCount = 0;
    impl->lastQueryReject = QueryReject::Count;
    impl->diagnostics.enabled = diagnosticsEnabled;
    // Upscalers may submit separate eye textures while rendering through the
    // bridge's shared scene depth. That exact depth resource identifies the
    // scene without a shared submitted color; SceneScope still checks every
    // bound DSV/MRT and Query requires fresh depth ownership before masking.
    if (!ctx || (!depth && !submitted) || width <= 0 || height <= 0) return false;
    auto fits = [width, height](const DensityMaskManager::EyeRegion& r) {
        return r.left >= 0 && r.top >= 0 && r.width > 0 && r.height > 0 &&
            std::int64_t(r.left)+r.width <= width && std::int64_t(r.top)+r.height <= height;
    };
    if (!fits(left) || !fits(right) ||
        (left.left < right.left+right.width && right.left < left.left+left.width &&
         left.top < right.top+right.height && right.top < left.top+left.height)) return false;
    ComPtr<ID3D11Device> device; ctx->GetDevice(&device);
    if (impl->device && impl->device != device) Shutdown();
    auto& p = *impl;
    p.diagnostics.enabled = diagnosticsEnabled;
    p.device = device; p.context = ctx; p.expectedDepth = depth; p.submitted = submitted;
    if (p.width != UINT(width) || p.height != UINT(height) ||
        std::memcmp(&p.regions[0], &left, sizeof(left)) || std::memcmp(&p.regions[1], &right, sizeof(right))) {
        p.depthOwner.Reset();
    }
    p.width = width; p.height = height; p.regions[0] = left; p.regions[1] = right;
    p.pattern = pattern; std::copy_n(centers, 4, p.centers); p.stats = {};
    if (!p.resolver.Initialize(device.Get()))return false;
    if (depth && !p.resolver.PrepareDepthGuide(depth))return false;
    if (!depth)p.resolver.ClearDepthGuide();
    p.scope.Arm(ctx, depth, submitted, width, height);
    if (!ocu_vrs_guard::InstallShaderCapture(device.Get()) || !Install(ctx) ||
        !ocu_vrs_guard::WatchContext(ctx, &ShaderChanged,
            +[](ID3D11DeviceContext* context) {
                if (auto* owner = RDMRenderScope::Active(context)) owner->BeforeCompute();
            })) return false;
    p.armed = true; p.dirty = true; p.guideState = {};
    p.gpuTiming.BeginFrame(ctx, diagnosticsEnabled);
    active.store(this, std::memory_order_release);
    activeContext.store(ctx, std::memory_order_release);
    return true;
}
void RDMRenderScope::EndFrame()
{
    impl->Finish(); impl->armed = false; impl->current = {}; impl->scope.Reset();
    impl->gpuTiming.EndFrame();
    impl->preparedMask = false;
    auto* expected = this;
    if (active.compare_exchange_strong(expected, nullptr)) {
        activeContext.store(nullptr, std::memory_order_release);
        ocu_vrs_guard::UnwatchContext(impl->context.Get());
    }
}
void RDMRenderScope::Shutdown()
{
    EndFrame();
    impl = std::make_unique<Impl>();
}
RDMRenderScope::Statistics RDMRenderScope::Stats() const { return impl->stats; }
bool RDMRenderScope::TakeGpuSample(GpuSample& result)
{
    const auto& sample = impl->gpuTiming.Latest();
    if (!sample || sample->sampleId <= impl->deliveredGpuSample) return false;
    result = {sample->sampleId, sample->preparationSegments, sample->resolveSegments,
        sample->preparationMs, sample->resolveMs, sample->frameSpanMs};
    for (unsigned i = 0; i < result.work.size(); ++i) {
        const auto& work = sample->work[i];
        result.work[i] = {work.calls, work.measuredCalls, work.cpuMs, work.gpuSubsetMs,
            work.vsInvocations, work.psInvocations};
    }
    result.framePipeline = sample->framePipeline;
    impl->deliveredGpuSample = sample->sampleId;
    return true;
}
const RDMRenderScope::DiagnosticSnapshot& RDMRenderScope::Diagnostics() const { return impl->diagnostics; }
void RDMRenderScope::StateChanged(ID3D11DeviceContext* ctx, unsigned guideChanges)
{
    if (ctx == impl->context.Get() && !impl->busy) {
        impl->dirty = true;
        if (guideChanges == GuideAll) impl->colorCoverageReady = false;
        if (guideChanges) { impl->guideState.dirty = true; impl->guideState.changed |= guideChanges; }
    }
}
void RDMRenderScope::BeforeDepthStateBoundary()
{
    if (!impl->busy) impl->RestoreOriginalDepth();
}
void RDMRenderScope::NotifyBeforeDepthStateBoundary(ID3D11DeviceContext* ctx)
{
    if (auto* owner = Active(ctx)) owner->BeforeDepthStateBoundary();
}
void RDMRenderScope::ExposeOriginalDepthBinding(ID3D11DepthStencilView** depth)
{
    auto& p = *impl;
    if (p.busy || !p.privateDepthBound || !depth || *depth != p.privateDSV.Get()) return;
    // Return the same owned reference the native getter would return for the
    // application's binding. Keep the physical private DSV for the next draw;
    // output mutations, consuming boundaries and incompatible draws restore
    // it before doing their native work.
    auto* original = p.batch.dsv.Get();
    if (original) original->AddRef();
    (*depth)->Release();
    *depth = original;
}
void RDMRenderScope::NotifyState(ID3D11DeviceContext* ctx, unsigned guideChanges)
{
    if (auto* owner = Active(ctx)) owner->StateChanged(ctx, guideChanges);
    if (const auto* observer = Observer(ctx); observer && observer->stateChanged)
        observer->stateChanged(observer->owner, ctx);
}
void RDMRenderScope::NotifyGeometryState(ID3D11DeviceContext* ctx)
{
    if (auto* owner = Active(ctx)) {
        owner->impl->colorCoverageReady = false;
        owner->StateChanged(ctx, GuideNone);
    }
}
void RDMRenderScope::NotifyTargets(ID3D11DeviceContext* ctx)
{
    NotifyState(ctx, GuideTargets | GuideUavs);
}
void RDMRenderScope::RegisterTargetObserver(void* renderTargets, void* renderTargetsAndUavs)
{ externalTargets.store(renderTargets); externalTargetsUav.store(renderTargetsAndUavs); }
bool RDMRenderScope::RegisterDrawObserver(ID3D11DeviceContext* ctx, const DrawObserver* observer)
{
    if (!ctx || !observer || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !Install(ctx)) return false;
    drawObserverContext.store(nullptr, std::memory_order_release);
    drawObserver = observer;
    drawObserverContext.store(ctx, std::memory_order_release);
    return true;
}
void RDMRenderScope::RemoveDrawObserver(ID3D11DeviceContext* ctx)
{
    if (ctx && ctx != drawObserverContext.load(std::memory_order_acquire)) return;
    drawObserverContext.store(nullptr, std::memory_order_release);
    drawObserver = nullptr;
}
bool RDMRenderScope::BeginColorCoverage(const std::array<std::uint64_t, 8>& key, bool reusable)
{
    auto& p = *impl;
    Impl::CpuTimer timer(p.diagnostics.enabled, p.stats.admissionCpuMs);
    if (p.dirty) p.eligible = p.Query();
    if (!p.eligible) { p.colorCoverageReady = false; return false; }
    if (reusable && p.colorCoverageReusable && p.colorCoverageReady && p.colorDrawKey == key) {
        ++p.stats.colorCoverageReuses;
        return false;
    }
    // Complete the previous color batch before replacing its ownership guide.
    p.Finish();
    p.busy = true;
    p.preparedMask = false;
    p.colorCoverageReady = false;
    p.colorDrawKey = key;
    p.colorCoverageReusable = reusable;
    p.capturedRasterGeometry = p.currentRasterGeometry;
    p.capturedCoverageDepth = p.current.depth;
    // Query has already excluded shader discard/depth/coverage side effects,
    // stencil, blending, predication, stream output, and UAV writes. Keep the
    // original EQUAL test and geometry stages; only replace color outputs.
    p.gpuTiming.BeginWork(ocu_rdm::GpuTiming::WorkStage::GuideCapture);
    p.resolver.ClearDepthGuide();
    if (!p.resolver.BeginDepthGuide(p.current.dsv.Get(), false)) {
        p.gpuTiming.EndWork(ocu_rdm::GpuTiming::WorkStage::GuideCapture);
        p.busy = false;
        return false;
    }
    return true;
}
void RDMRenderScope::EndColorCoverage()
{
    auto& p = *impl;
    p.resolver.EndDepthGuide();
    p.gpuTiming.EndWork(ocu_rdm::GpuTiming::WorkStage::GuideCapture);
    p.busy = false;
    p.colorCoverageReady = true;
    ++p.stats.colorCoverageDraws;
}
bool RDMRenderScope::BeforeDraw()
{
    auto& p = *impl; ++p.stats.draws;
    Impl::CpuTimer timer(p.diagnostics.enabled, p.stats.admissionCpuMs);
    if (p.dirty) p.eligible = p.Query();
    if (!p.eligible || !p.colorCoverageReady) {
        p.Finish(); ++p.stats.protectedDraws;
        if (p.lastQueryReject != QueryReject::Count)
            ++p.stats.rejectedDrawCounts[static_cast<unsigned>(p.lastQueryReject)];
        return false;
    }
    if (p.pending && !p.SameBatch()) p.Finish();
    if (!p.pending && !p.Start()) {
        ++p.stats.protectedDraws; ++p.stats.startRejectedDraws;
        if (p.diagnostics.enabled && !p.diagnostics.start.valid)
            p.CaptureDiagnostic(p.diagnostics.start, QueryReject::Count, p.stats.lastStartFailure, true);
        return false;
    }
    p.busy = true;
    if (!p.privateDepthBound) {
        p.Bind(p.privateDSV.Get());
        p.privateDepthBound = true;
        ++p.stats.privateDepthBinds;
    }
    ++p.stats.maskedDraws;
    p.stats.partialMaskDraws += p.current.partialWrites;
    p.stats.inactiveTargetDraws += p.current.inactiveTargets;
    return true;
}
void RDMRenderScope::AfterDraw(bool masked)
{
    auto& p = *impl;
    // Consecutive compatible draws share private depth. Getters expose the
    // logical game binding; output setters and consumers restore it physically.
    if (masked) p.busy = false;
}
void RDMRenderScope::BeforeRead(ID3D11Resource* resource)
{
    auto& p = *impl;
    if (!p.pending || !resource) return;
    bool match = resource == p.batch.depth.Get();
    for (const auto& color : p.batch.colors) match = match || resource == color.Get();
    if (match) { ++p.stats.consumerBoundaries; p.Finish(); p.dirty = true; }
}
void RDMRenderScope::InvalidateDepthGuide()
{
    auto& p=*impl;
    p.preparedMask=false;
    p.colorCoverageReady=false;
    Impl::Busy guard(p);p.resolver.ClearDepthGuide();p.dirty=true;++p.stats.guideResets;
}
void RDMRenderScope::BeforeWrite(ID3D11Resource* resource)
{
    // Readback destinations cannot supply geometry or depth. Do not discard
    // valid coverage just because a consumer copies color into staging memory.
    bool staging = false;
    if (resource) {
        D3D11_RESOURCE_DIMENSION type{}; resource->GetType(&type);
        if (type == D3D11_RESOURCE_DIMENSION_BUFFER) {
            ComPtr<ID3D11Buffer> buffer;
            if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&buffer)))) {
                D3D11_BUFFER_DESC d{}; buffer->GetDesc(&d); staging = d.Usage == D3D11_USAGE_STAGING;
            }
        } else if (type == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) {
                D3D11_TEXTURE2D_DESC d{}; texture->GetDesc(&d); staging = d.Usage == D3D11_USAGE_STAGING;
            }
        }
    }
    if (!staging) impl->colorCoverageReady = false;
    BeforeRead(resource);
    if(resource && resource==impl->expectedDepth.Get())InvalidateDepthGuide();
}
bool RDMRenderScope::BeginDepthGuide(bool invalidate)
{
    auto& p=*impl;
    if(!p.expectedDepth || !p.resolver.DepthGuide() || p.busy)return false;
    if(invalidate && !p.resolver.HasDepthGuideDraws()) { ++p.stats.emptyGuideSkips; return false; }
    if(p.guideState.dirty)p.QueryGuide();
    if(!p.guideState.candidate || p.guideState.invalidate!=invalidate)return false;
    if(p.guideState.clear) {
        if(p.resolver.HasDepthGuideDraws())InvalidateDepthGuide();
        return false;
    }
    p.busy=true;
    p.invalidatingGuide = invalidate;
    p.gpuTiming.BeginWork(invalidate ? ocu_rdm::GpuTiming::WorkStage::GuideInvalidation :
        ocu_rdm::GpuTiming::WorkStage::GuideCapture);
    if(!p.resolver.BeginDepthGuide(p.guideState.view.Get(),invalidate)) {
        p.gpuTiming.EndWork(invalidate ? ocu_rdm::GpuTiming::WorkStage::GuideInvalidation :
            ocu_rdm::GpuTiming::WorkStage::GuideCapture);
        p.busy=false;InvalidateDepthGuide();return false;
    }
    if(invalidate)++p.stats.guideInvalidationDraws;else ++p.stats.guideDraws;
    return true;
}
void RDMRenderScope::EndDepthGuide()
{
    auto& p=*impl;p.resolver.EndDepthGuide();
    p.gpuTiming.EndWork(p.invalidatingGuide ? ocu_rdm::GpuTiming::WorkStage::GuideInvalidation :
        ocu_rdm::GpuTiming::WorkStage::GuideCapture);
    p.busy=false;
    p.preparedMask=false;
    p.colorCoverageReady=false;
    // Guide bindings are fully restored. Only guide availability can change
    // color admission without a game pipeline-state setter notifying us.
    if(p.lastQueryReject==QueryReject::NoDepthGuide)p.dirty=true;
}
void RDMRenderScope::BeginSceneDraw(bool masked)
{
    if (impl->diagnostics.enabled) impl->gpuTiming.BeginWork(masked ?
        ocu_rdm::GpuTiming::WorkStage::SceneMasked : ocu_rdm::GpuTiming::WorkStage::SceneFullRate);
}
void RDMRenderScope::EndSceneDraw(bool masked)
{
    if (impl->diagnostics.enabled) impl->gpuTiming.EndWork(masked ?
        ocu_rdm::GpuTiming::WorkStage::SceneMasked : ocu_rdm::GpuTiming::WorkStage::SceneFullRate);
}
void RDMRenderScope::BeforeReads(UINT count, ID3D11ShaderResourceView* const* views)
{
    if (!impl->pending || !views) return;
    for (UINT i = 0; i < count && impl->pending; ++i) if (views[i]) {
        ComPtr<ID3D11Resource> resource; views[i]->GetResource(&resource); BeforeRead(resource.Get());
    }
}
void RDMRenderScope::BeforeCompute()
{
    impl->colorCoverageReady = false;
    if (impl->pending) { ++impl->stats.consumerBoundaries; impl->Finish(); }
    // UAV bindings and command execution can change implicit resource hazards.
    impl->dirty = true; impl->guideState.dirty = true; impl->guideState.changed = GuideAll;
}
