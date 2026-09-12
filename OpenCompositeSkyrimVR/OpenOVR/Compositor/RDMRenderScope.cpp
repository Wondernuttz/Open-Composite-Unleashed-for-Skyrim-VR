#include "RDMRenderScope.h"
#include "VRSShaderGuard.h"
#include "VRSSceneScope.h"
#include "../logging.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <atomic>
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
template<class Function, class Handler> struct Hook;
template<class Handler, class Result, class... Args>
struct Hook<Result(STDMETHODCALLTYPE*)(Args...), Handler> {
    using Function = Result(STDMETHODCALLTYPE*)(Args...);
    static inline std::array<void*, 8> targets{};
    static inline std::array<Function, 8> originals{};
    static inline std::mutex lock;
    template<size_t I> static Result STDMETHODCALLTYPE Detour(Args... args)
    { return Handler::Call(originals[I], args...); }
    template<size_t... I> static auto Detours(std::index_sequence<I...>)
    { return std::array<Function, sizeof...(I)>{&Detour<I>...}; }
    static bool Install(void* target)
    {
        std::lock_guard<std::mutex> guard(lock);
        for (auto p : targets) if (p == target) return true;
        auto detours = Detours(std::make_index_sequence<8>{});
        for (size_t i = 0; i < targets.size(); ++i) if (!targets[i]) {
            if (MH_CreateHook(target, reinterpret_cast<void*>(detours[i]),
                    reinterpret_cast<void**>(&originals[i])) != MH_OK) return false;
            if (MH_EnableHook(target) != MH_OK) {
                MH_RemoveHook(target); originals[i] = nullptr; return false;
            }
            targets[i] = target; return true;
        }
        return false;
    }
};
struct DrawHandler {
    template<class F, class... A> static void Call(F original, ID3D11DeviceContext* ctx, A... args)
    {
        auto* owner = RDMRenderScope::Active(ctx);
        bool hasWork = true;
        if constexpr (sizeof...(A) > 0) {
            const auto values = std::forward_as_tuple(args...);
            if constexpr (std::is_integral_v<std::remove_reference_t<decltype(std::get<0>(values))>>) {
                hasWork = std::get<0>(values) != 0;
                if constexpr (sizeof...(A) >= 4) hasWork = hasWork && std::get<1>(values) != 0;
            }
        }
        const auto* observer = hasWork && !observerDrawDepth ? Observer(ctx) : nullptr;
        if (observer) {
            ++observerDrawDepth;
            if (observer->beforeDraw) observer->beforeDraw(observer->owner, ctx);
        }
        const bool masked = owner && owner->BeforeDraw();
        const bool guide = owner && !masked && owner->BeginDepthGuide(false);
        original(ctx, args...);
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
struct StateHandler {
    template<class F, class... A> static void Call(F original, ID3D11DeviceContext* ctx, A... args)
    {
        original(ctx, args...);
        RDMRenderScope::NotifyTargets(ctx);
    }
};
struct ReadHandler {
    template<class F> static void Call(F original, ID3D11DeviceContext* ctx, UINT slot,
        UINT count, ID3D11ShaderResourceView* const* views)
    {
        if (auto* owner = RDMRenderScope::Active(ctx)) owner->BeforeReads(count, views);
        original(ctx, slot, count, views);
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
void ShaderChanged(ID3D11DeviceContext* ctx, bool)
{
    if (auto* o = RDMRenderScope::Active(ctx)) {
        if (ocu_vrs_guard::CurrentReasons(ctx) & ocu_vrs_guard::CommandList) {
            o->BeforeCompute();o->InvalidateDepthGuide();
        }
        o->StateChanged(ctx);
    }
}
bool Install(ID3D11DeviceContext* ctx)
{
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) return false;
    // Hook the driver's methods below any game/CSX vtable wrapper. Its
    // post-draw observers must see the original DSV restored by AfterDraw.
    struct NativeDevice { LUID adapter{}; ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; };
    static std::array<NativeDevice, 8> nativeDevices;
    static std::mutex nativeLock;
    std::lock_guard<std::mutex> lock(nativeLock);
    ComPtr<ID3D11Device> gameDevice; ctx->GetDevice(&gameDevice);
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(gameDevice.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return false;
    ID3D11DeviceContext* native = nullptr;
    for (auto& d : nativeDevices) if (d.device && d.adapter.LowPart == desc.AdapterLuid.LowPart &&
        d.adapter.HighPart == desc.AdapterLuid.HighPart &&
        d.device->GetCreationFlags() == gameDevice->GetCreationFlags() &&
        d.device->GetFeatureLevel() == gameDevice->GetFeatureLevel()) { native = d.context.Get(); break; }
    if (!native) for (auto& d : nativeDevices) if (!d.device) {
        const D3D_FEATURE_LEVEL levels[] = {gameDevice->GetFeatureLevel()};
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, gameDevice->GetCreationFlags(),
            levels, 1, D3D11_SDK_VERSION, &d.device, nullptr, &d.context);
        if (FAILED(hr) && desc.VendorId == 0x1414)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, gameDevice->GetCreationFlags(),
                levels, 1, D3D11_SDK_VERSION, &d.device, nullptr, &d.context);
        if (FAILED(hr)) return false;
        d.adapter = desc.AdapterLuid; native = d.context.Get(); break;
    }
    if (!native) return false;
    auto v = *reinterpret_cast<void***>(native);
    bool ok = true;
#define INSTALL(slot, handler, ...) ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, __VA_ARGS__), handler>::Install(v[slot]) && ok
    if (v[33] != externalTargets.load()) {
        INSTALL(33, StateHandler, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
    }
    if (v[34] != externalTargetsUav.load()) {
        INSTALL(34, StateHandler, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
    }
    INSTALL(12, DrawHandler, UINT, UINT, INT);
    INSTALL(13, DrawHandler, UINT, UINT);
    INSTALL(20, DrawHandler, UINT, UINT, UINT, INT, UINT);
    INSTALL(21, DrawHandler, UINT, UINT, UINT, UINT);
    ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*), DrawHandler>::Install(v[38]) && ok;
    INSTALL(39, DrawHandler, ID3D11Buffer*, UINT);
    INSTALL(40, DrawHandler, ID3D11Buffer*, UINT);
    INSTALL(36, StateHandler, ID3D11DepthStencilState*, UINT);
    INSTALL(43, StateHandler, ID3D11RasterizerState*);
    // VRSShaderGuard owns RSSetViewports and forwards it through ShaderChanged.
    // A second MinHook detour here prevents both RDM and shared VRS hooks arming.
    INSTALL(45, StateHandler, UINT, const D3D11_RECT*);
    INSTALL(30, StateHandler, ID3D11Predicate*, BOOL);
    INSTALL(37, StateHandler, UINT, ID3D11Buffer* const*, const UINT*);
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
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
        auto v1 = *reinterpret_cast<void***>(ctx1.Get());
#define INSTALL1(slot, ...) ok = Hook<void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, __VA_ARGS__), ResourceWriteHandler>::Install(v1[slot]) && ok
        INSTALL1(115, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
        INSTALL1(116, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
        INSTALL1(117, ID3D11Resource*);
        INSTALL1(118, ID3D11View*);
        INSTALL1(132, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT);
        INSTALL1(133, ID3D11View*, const D3D11_RECT*, UINT);
#undef INSTALL1
    }
    ok = Hook<HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP,
        UINT, D3D11_MAPPED_SUBRESOURCE*), MapHandler>::Install(v[14]) && ok;
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
    ocu_vrs_scope::SceneScope scope;
    DensityMaskManager::EyeRegion regions[2];
    DensityMaskManager::PatternSettings pattern;
    float centers[4]{};
    unsigned width = 0, height = 0;
    bool armed = false, busy = false, dirty = true, eligible = false, pending = false;
    Statistics stats;
    struct Targets {
        std::array<ComPtr<ID3D11RenderTargetView>, 8> views;
        std::array<ComPtr<ID3D11Texture2D>, 8> colors;
        ComPtr<ID3D11DepthStencilView> dsv;
        ComPtr<ID3D11Texture2D> depth;
        UINT count = 0, eyes = 0;
    } current, batch;
    struct Busy {
        Impl& p; bool was;
        Busy(Impl& p) : p(p), was(p.busy) { p.busy = true; }
        ~Busy() { p.busy = was; }
    };
    void Bind(ID3D11DepthStencilView* depth)
    {
        ID3D11RenderTargetView* views[8]{};
        for (UINT i = 0; i < current.count; ++i) views[i] = current.views[i].Get();
        context->OMSetRenderTargets(current.count, views, depth);
    }
    bool Query()
    {
        ++stats.stateQueries;
        current = {}; dirty = false;
        ID3D11RenderTargetView* views[8]{};
        context->OMGetRenderTargets(8, views, &current.dsv);
        for (unsigned i = 0; i < 8; ++i) { current.views[i].Attach(views[i]); if (views[i]) current.count = i + 1; }
        if (!scope.Matches(context.Get(), 8, views, current.dsv.Get()) ||
            ocu_vrs_guard::CurrentReasons(context.Get()) != ocu_vrs_guard::Compatible) return false;
        if (!resolver.HasDepthGuideDraws()) { ++stats.noGuideStates; return false; }
        ComPtr<ID3D11DepthStencilState> state;
        context->OMGetDepthStencilState(&state, nullptr);
        if (!state) return false;
        D3D11_DEPTH_STENCIL_DESC depth{}; state->GetDesc(&depth);
        // Equality cannot create new depth, even with depth writes enabled.
        // Blending and stencil-dependent coverage require full-rate rendering.
        if (!depth.DepthEnable || depth.DepthFunc != D3D11_COMPARISON_EQUAL ||
            depth.StencilEnable) return false;
        ComPtr<ID3D11BlendState> blend; UINT sampleMask = 0;
        context->OMGetBlendState(&blend, nullptr, &sampleMask);
        if (sampleMask != 0xffffffffu) return false;
        ComPtr<ID3D11PixelShader> ps; context->PSGetShader(&ps, nullptr, nullptr);
        const auto outputs = ocu_vrs_guard::ShaderColorOutputs(ps.Get());
        for (unsigned i = 0; i < current.count; ++i)
            if (views[i] && !(outputs & (1u << i))) return false;
        ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{};
        context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT, streams);
        bool hasStream = false;
        for (auto* stream : streams) if (stream) { hasStream = true; stream->Release(); }
        if (hasStream) return false;
        if (blend) {
            D3D11_BLEND_DESC b{}; blend->GetDesc(&b);
            if (b.AlphaToCoverageEnable) return false;
            ComPtr<ID3D11BlendState1> blend1;
            if (SUCCEEDED(blend.As(&blend1))) {
                D3D11_BLEND_DESC1 b1{}; blend1->GetDesc1(&b1);
                for (unsigned i = 0; i < current.count; ++i)
                    if (views[i] && b1.RenderTarget[b1.IndependentBlendEnable ? i : 0].LogicOpEnable) return false;
            }
            for (unsigned i = 0; i < current.count; ++i) if (views[i]) {
                const auto& target = b.RenderTarget[b.IndependentBlendEnable ? i : 0];
                if (target.BlendEnable || target.RenderTargetWriteMask != D3D11_COLOR_WRITE_ENABLE_ALL) return false;
            }
        }
        ComPtr<ID3D11Predicate> predicate; context->GetPredication(&predicate, nullptr);
        if (predicate) return false;
        ComPtr<ID3D11Resource> resource; current.dsv->GetResource(&resource);
        if (FAILED(resource.As(&current.depth))) return false;
        D3D11_TEXTURE2D_DESC dd{}; current.depth->GetDesc(&dd);
        if (!(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || dd.MipLevels != 1) return false;
        for (unsigned i = 0; i < current.count; ++i) if (views[i]) {
            resource.Reset(); views[i]->GetResource(&resource);
            if (FAILED(resource.As(&current.colors[i]))) return false;
            D3D11_TEXTURE2D_DESC td{}; current.colors[i]->GetDesc(&td);
            if (td.MipLevels != 1 || !SupportedColor(td.Format)) return false;
        }
        // An OM UAV is an observable side effect even when the current shader
        // does not declare it. Do not disturb bindings or hidden UAV counters.
        const UINT slots = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? 64 : 8;
        ID3D11UnorderedAccessView* uavs[64]{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
            current.count, slots-current.count, uavs+current.count);
        bool hasUav = false;
        for (auto* uav : uavs) if (uav) { hasUav = true; uav->Release(); }
        if (hasUav) return false;
        D3D11_VIEWPORT vp[16]{}; UINT count = 16;
        context->RSGetViewports(&count, vp);
        ComPtr<ID3D11RasterizerState> rs; context->RSGetState(&rs);
        D3D11_RASTERIZER_DESC rd{}; if (rs) rs->GetDesc(&rd);
        if (rs && (rd.FillMode != D3D11_FILL_SOLID || rd.DepthBias || rd.SlopeScaledDepthBias != 0 || !rd.DepthClipEnable)) return false;
        D3D11_RECT scissors[16]{}; UINT sc = 16;
        if (rd.ScissorEnable) context->RSGetScissorRects(&sc, scissors);
        for (int eye = 0; eye < 2; ++eye) {
            const auto& e = regions[eye];
            for (UINT v = 0; v < count; ++v) {
                if (vp[v].MinDepth != 0 || vp[v].MaxDepth != 1) continue;
                bool covers = vp[v].TopLeftX <= e.left && vp[v].TopLeftY <= e.top &&
                    vp[v].TopLeftX+vp[v].Width >= e.left+e.width && vp[v].TopLeftY+vp[v].Height >= e.top+e.height;
                if (rd.ScissorEnable) covers = covers && v < sc && scissors[v].left <= e.left &&
                    scissors[v].top <= e.top && scissors[v].right >= e.left+e.width && scissors[v].bottom >= e.top+e.height;
                if (covers) current.eyes |= 1u << eye;
            }
        }
        return current.eyes != 0;
    }
    bool SameBatch() const
    {
        if (batch.depth != current.depth || batch.eyes != current.eyes || batch.count != current.count) return false;
        for (unsigned i = 0; i < 8; ++i) if (batch.views[i] != current.views[i]) return false;
        return true;
    }
    bool PrepareDepth()
    {
        if (depthOwner == current.depth && privateDepth && depthSRV && eligibilityCB && eligibilityCS) return true;
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
        default: return false;
        }
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(current.depth.Get(), &sv, &depthSRV))) return false;
        d.BindFlags = D3D11_BIND_DEPTH_STENCIL; d.MiscFlags = 0;
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0; dv.Flags = 0;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &privateDepth)) ||
            FAILED(device->CreateDepthStencilView(privateDepth.Get(), &dv, &privateDSV))) return false;
        d.Format = DXGI_FORMAT_R8_UNORM; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &coverage)) ||
            FAILED(device->CreateRenderTargetView(coverage.Get(), nullptr, &coverageRTV)) ||
            FAILED(device->CreateShaderResourceView(coverage.Get(), nullptr, &coverageSRV))) return false;
        eligibility.Reset(); eligibilitySRV.Reset(); eligibilityUAV.Reset();
        d.Width = (regions[0].width+7)/8 + (regions[1].width+7)/8;
        d.Height = (std::max(regions[0].height, regions[1].height)+7)/8;
        d.Format = DXGI_FORMAT_R32_FLOAT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &eligibility)) ||
            FAILED(device->CreateShaderResourceView(eligibility.Get(), nullptr, &eligibilitySRV)) ||
            FAILED(device->CreateUnorderedAccessView(eligibility.Get(), nullptr, &eligibilityUAV))) return false;
        if (!eligibilityCS || !eligibilityCB) {
            eligibilityCS.Reset(); eligibilityCB.Reset();
            ComPtr<ID3DBlob> code, errors;
            HRESULT hr = D3DCompile(depthEligibilityShader, sizeof(depthEligibilityShader)-1,
                "OCU RDM depth eligibility", nullptr, nullptr, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
            if (FAILED(hr) || FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &eligibilityCS))) return false;
            D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 32; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.Usage = D3D11_USAGE_DYNAMIC; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&cb, nullptr, &eligibilityCB))) return false;
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
            if (FAILED(context->Map(input, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) { ok = false; break; }
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
        Busy guard(*this);
        if (!PrepareDepth()) return false;
        if (!resolver.Initialize(device.Get())) return false;
        resolver.SetPatternSettings(pattern);
        resolver.SetProjectionCenters(centers[0], centers[1], centers[2], centers[3]);
        ID3D11Texture2D* colors[8]{};
        for (unsigned i=0;i<current.count;++i)colors[i]=current.colors[i].Get();
        if (!resolver.PrepareMRTTargets(colors,current.count,width,height,regions[0],regions[1]))return false;
        context->CopyResource(privateDepth.Get(), current.depth.Get());
        if (!BuildEligibility()) return false;
        const float zero[4]{}; context->ClearRenderTargetView(coverageRTV.Get(), zero);
        if (!resolver.ApplyDepthMask(privateDSV.Get(), 1.0f, coverageRTV.Get(), eligibilitySRV.Get())) return false;
        batch = current; pending = true; ++stats.batches;
        return true;
    }
    void Finish()
    {
        if (!pending) return;
        Busy guard(*this);
        pending = false;
        const bool ok = resolver.ResolveMRTs(coverageSRV.Get(), batch.eyes);
        for (unsigned i=0;i<batch.count;++i)if(batch.colors[i])++stats.resolves;
        if (!ok) {
            armed = false;
            OOVR_LOG("RDM handoff: GPU color resolve failed; inspect device status");
        }
        batch = {};
    }
};

RDMRenderScope::RDMRenderScope() : impl(std::make_unique<Impl>()) {}
RDMRenderScope::~RDMRenderScope() { EndFrame(); }
RDMRenderScope* RDMRenderScope::Active(ID3D11DeviceContext* ctx)
{
    if (ctx != activeContext.load(std::memory_order_acquire)) return nullptr;
    auto* owner = active.load(std::memory_order_acquire);
    return owner && owner->impl->context.Get() == ctx && owner->impl->armed && !owner->impl->busy ? owner : nullptr;
}
bool RDMRenderScope::Arm(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth,
    ID3D11Texture2D* submitted, int width, int height,
    const DensityMaskManager::EyeRegion& left, const DensityMaskManager::EyeRegion& right,
    const DensityMaskManager::PatternSettings& pattern, const float centers[4])
{
    EndFrame();
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
        !ocu_vrs_guard::WatchContext(ctx, &ShaderChanged)) return false;
    p.armed = true; p.dirty = true;
    active.store(this, std::memory_order_release);
    activeContext.store(ctx, std::memory_order_release);
    return true;
}
void RDMRenderScope::EndFrame()
{
    impl->Finish(); impl->armed = false; impl->current = {}; impl->scope.Reset();
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
void RDMRenderScope::StateChanged(ID3D11DeviceContext* ctx)
{ if (ctx == impl->context.Get() && !impl->busy) impl->dirty = true; }
void RDMRenderScope::NotifyTargets(ID3D11DeviceContext* ctx)
{
    if (auto* owner = Active(ctx)) owner->StateChanged(ctx);
    if (const auto* observer = Observer(ctx); observer && observer->stateChanged)
        observer->stateChanged(observer->owner, ctx);
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
bool RDMRenderScope::BeforeDraw()
{
    auto& p = *impl; ++p.stats.draws;
    if (p.dirty) p.eligible = p.Query();
    if (!p.eligible) { p.Finish(); ++p.stats.protectedDraws; return false; }
    if (p.pending && !p.SameBatch()) p.Finish();
    if (!p.pending && !p.Start()) { ++p.stats.protectedDraws; return false; }
    p.busy = true; p.Bind(p.privateDSV.Get());
    ++p.stats.maskedDraws;
    return true;
}
void RDMRenderScope::AfterDraw(bool masked)
{
    auto& p = *impl;
    if (masked) { p.Bind(p.current.dsv.Get()); p.busy = false; }
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
    Impl::Busy guard(p);p.resolver.ClearDepthGuide();p.dirty=true;++p.stats.guideResets;
}
void RDMRenderScope::BeforeWrite(ID3D11Resource* resource)
{
    BeforeRead(resource);
    if(resource && resource==impl->expectedDepth.Get())InvalidateDepthGuide();
}
bool RDMRenderScope::BeginDepthGuide(bool invalidate)
{
    auto& p=*impl;
    if(!p.expectedDepth || !p.resolver.DepthGuide() || p.busy)return false;
    ComPtr<ID3D11DepthStencilState> state;p.context->OMGetDepthStencilState(&state,nullptr);
    D3D11_DEPTH_STENCIL_DESC ds{};
    if(state)state->GetDesc(&ds);
    else {ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_LESS;}
    // Equality writes reproduce existing depth; they do not introduce a new owner.
    if(!ds.DepthEnable || ds.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ALL ||
        ds.DepthFunc==D3D11_COMPARISON_EQUAL || ds.DepthFunc==D3D11_COMPARISON_NEVER)return false;
    ComPtr<ID3D11DepthStencilView> dsv;p.context->OMGetRenderTargets(0,nullptr,&dsv);
    ComPtr<ID3D11Resource> resource;if(dsv)dsv->GetResource(&resource);
    if(resource.Get()!=p.expectedDepth.Get())return false;
    ComPtr<ID3D11PixelShader> ps;p.context->PSGetShader(&ps,nullptr,nullptr);
    if(!invalidate && ps)return false;
    if(invalidate && !ps)return false;
    const auto reasons=ocu_vrs_guard::CurrentReasons(p.context.Get());
    ComPtr<ID3D11Predicate> predicate;p.context->GetPredication(&predicate,nullptr);
    ComPtr<ID3D11BlendState> blend;UINT sampleMask=0;p.context->OMGetBlendState(&blend,nullptr,&sampleMask);
    D3D11_BLEND_DESC bd{};if(blend)blend->GetDesc(&bd);
    ID3D11Buffer* streams[D3D11_SO_BUFFER_SLOT_COUNT]{};p.context->SOGetTargets(D3D11_SO_BUFFER_SLOT_COUNT,streams);
    bool streamOutput=false;for(auto* stream:streams)if(stream){streamOutput=true;stream->Release();}
    ID3D11UnorderedAccessView* uavs[64]{};
    const UINT uavCount=p.device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
    p.context->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,uavCount,uavs);
    bool unorderedAccess=false;for(auto* uav:uavs)if(uav){unorderedAccess=true;uav->Release();}
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dsv->GetDesc(&dv);
    const unsigned forbidden=ocu_vrs_guard::Unclassified|ocu_vrs_guard::DepthOrCoverage|
        ocu_vrs_guard::UnorderedAccess|ocu_vrs_guard::ClassLinkage|ocu_vrs_guard::CommandList;
    if(predicate || streamOutput || unorderedAccess || bd.AlphaToCoverageEnable || sampleMask!=0xffffffffu ||
        (dv.Flags&D3D11_DSV_READ_ONLY_DEPTH) || (reasons&forbidden)) {
        InvalidateDepthGuide();return false;
    }
    p.busy=true;
    if(!p.resolver.BeginDepthGuide(dsv.Get(),invalidate)) {
        p.busy=false;InvalidateDepthGuide();return false;
    }
    if(invalidate)++p.stats.guideInvalidationDraws;else ++p.stats.guideDraws;
    return true;
}
void RDMRenderScope::EndDepthGuide()
{
    auto& p=*impl;p.resolver.EndDepthGuide();p.busy=false;p.dirty=true;
}
void RDMRenderScope::BeforeReads(UINT count, ID3D11ShaderResourceView* const* views)
{
    if (!impl->pending || !views) return;
    for (UINT i = 0; i < count && impl->pending; ++i) if (views[i]) {
        ComPtr<ID3D11Resource> resource; views[i]->GetResource(&resource); BeforeRead(resource.Get());
    }
}
void RDMRenderScope::BeforeCompute()
{ if (impl->pending) { ++impl->stats.consumerBoundaries; impl->Finish(); impl->dirty = true; } }
