// The production producer is extracted without edits. These counted COM
// stand-ins test ownership/call counts, not GPU performance or driver behavior.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace {
std::atomic<unsigned> adds{0}, releases{0}, descriptions{0}, warnings{0};
unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::abort();
    }
}
struct CountedResource {
    std::atomic<unsigned> refs{1};
    void AddRef() { ++refs; ++adds; }
    void Release() { Check(refs.load() > 1, "published refs never consume renderer ownership"); --refs; ++releases; }
};
struct D3D11_TEXTURE2D_DESC { uint32_t Width = 0, Height = 0; };
struct ID3D11Texture2D : CountedResource {
    uint32_t width, height;
    ID3D11Texture2D(uint32_t w = 2240, uint32_t h = 2048) : width(w), height(h) {}
    void GetDesc(D3D11_TEXTURE2D_DESC* desc) { ++descriptions; desc->Width = width; desc->Height = height; }
};
struct ID3D11ShaderResourceView : CountedResource {};
struct ID3D11UnorderedAccessView : CountedResource {};
struct ID3D11Device : CountedResource {};
struct ID3D11DeviceContext : CountedResource {};
namespace RE {
struct RENDER_TARGET { static constexpr size_t kMOTION_VECTOR = 0; };
struct RENDER_TARGET_DEPTHSTENCIL { static constexpr size_t kMAIN = 0; };
namespace BSGraphics {
struct Renderer {
    struct RenderTarget { ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* SRV = nullptr; ID3D11UnorderedAccessView* UAV = nullptr; };
    struct DepthTarget { ID3D11Texture2D* texture = nullptr; ID3D11ShaderResourceView* depthSRV = nullptr; };
    struct RuntimeData { RenderTarget renderTargets[1]; ID3D11Device* forwarder = nullptr; ID3D11DeviceContext* context = nullptr; } runtime;
    struct DepthData { DepthTarget depthStencils[1]; } depth;
    inline static Renderer* singleton = nullptr;
    static Renderer* GetSingleton() { return singleton; }
    RuntimeData& GetRuntimeData() { return runtime; }
    DepthData& GetDepthStencilData() { return depth; }
};
}
}
namespace SKSE::log {
template<class... Args> void info(const char*, Args&&...) {}
template<class... Args> void warn(const char*, Args&&...) { ++warnings; }
}

#include "BridgeRefreshProduction.inl"

struct Fixture {
    alignas(8) OCRenderTargetBridge bridge{};
    RE::BSGraphics::Renderer renderer;
    ID3D11Texture2D mv, depth;
    ID3D11ShaderResourceView mvView, depthView;
    ID3D11UnorderedAccessView mvUav;
    ID3D11Device device;
    ID3D11DeviceContext context;
    Fixture()
    {
        g_pBridge = &bridge;
        g_bridgeResourceGeneration = 0;
        RE::BSGraphics::Renderer::singleton = &renderer;
        renderer.runtime.renderTargets[0] = {&mv, &mvView, &mvUav};
        renderer.runtime.forwarder = &device;
        renderer.runtime.context = &context;
        renderer.depth.depthStencils[0] = {&depth, &depthView};
    }
    ~Fixture()
    {
        g_bridgeResources.Reset();
        g_pBridge = nullptr;
        RE::BSGraphics::Renderer::singleton = nullptr;
    }
};

void AssertUnchanged(unsigned repeats)
{
    const auto before = *g_pBridge;
    const auto a = adds.load(), r = releases.load(), d = descriptions.load(), w = warnings.load();
    for (unsigned i = 0; i != repeats; ++i)
        RefreshBridgeRenderTargets();
    Check(adds == a, "unchanged refresh performs no AddRef");
    Check(releases == r, "unchanged refresh performs no Release");
    Check(descriptions == d, "unchanged refresh performs no GetDesc");
    Check(warnings == w, "unchanged refresh does not repeat warnings");
    Check(std::memcmp(&before, g_pBridge, sizeof(before)) == 0, "unchanged refresh does not rewrite bridge metadata");
}

void InitialAndSteadyState()
{
    Fixture f;
    RefreshBridgeRenderTargets();
    Check(f.bridge.status == 1 && f.bridge.resourceGeneration == 1, "first available set is published ready");
    Check(f.mv.refs == 2 && f.depth.refs == 2, "published textures retained exactly once");
    Check(f.bridge.publishSequence == 2, "initial publication leaves stable sequence");
    Check(f.bridge.mvWidth == 2240 && f.bridge.depthHeight == 2048, "immutable descriptors captured at publication");
    AssertUnchanged(1250); // Ten seconds of probes at the scheduler's 8 ms cadence.
    auto copy = AcquirePublishedBridgeResources();
    Check(f.mv.refs == 3 && f.depth.refs == 3, "local acquisition still retains the published resources");
    AssertUnchanged(10);
    copy.Reset();
    Check(f.mv.refs == 2 && f.depth.refs == 2, "local snapshot releases only its references");
}

void Replacements()
{
    // The extra resources outlive the fixture's final ownership release.
    ID3D11Texture2D depth2, mv2(3200, 2400);
    ID3D11ShaderResourceView mvView2, depthView2;
    ID3D11UnorderedAccessView mvUav2;
    ID3D11Device device2;
    ID3D11DeviceContext context2;
    Fixture f;
    RefreshBridgeRenderTargets();
    f.renderer.depth.depthStencils[0].texture = &depth2;
    const auto desc = descriptions.load();
    RefreshBridgeRenderTargets();
    Check(f.bridge.resourceGeneration == 2 && f.bridge.depthTexture == reinterpret_cast<uint64_t>(&depth2), "same-sized depth replacement publishes on next probe");
    Check(f.depth.refs == 1 && depth2.refs == 2, "obsolete depth retires and replacement is retained");
    Check(descriptions == desc + 2, "changed set describes its textures once");
    AssertUnchanged(10);

    auto change = [&](auto& slot, auto* replacement, const char* reason) {
        const auto generation = f.bridge.resourceGeneration;
        slot = replacement;
        RefreshBridgeRenderTargets();
        Check(f.bridge.resourceGeneration == generation + 1, reason);
        AssertUnchanged(10);
    };
    change(f.renderer.depth.depthStencils[0].depthSRV, &depthView2, "depth SRV replacement republishes");
    change(f.renderer.runtime.renderTargets[0].texture, &mv2, "MV texture replacement republishes");
    Check(f.bridge.mvWidth == 3200 && f.bridge.mvHeight == 2400, "replacement dimensions update");
    change(f.renderer.runtime.renderTargets[0].SRV, &mvView2, "MV SRV replacement republishes");
    change(f.renderer.runtime.renderTargets[0].UAV, &mvUav2, "MV UAV replacement republishes");
    change(f.renderer.runtime.forwarder, &device2, "device replacement republishes");
    change(f.renderer.runtime.context, &context2, "context replacement republishes");
}

void UnavailableAndRecovery()
{
    Fixture f;
    auto unavailable = [&] {
        const auto a = adds.load(), d = descriptions.load(), w = warnings.load();
        const auto generation = f.bridge.resourceGeneration;
        RefreshBridgeRenderTargets();
        Check(f.bridge.status == 2 && f.bridge.resourceGeneration == generation + 1, "missing required resource publishes unavailable once");
        Check(!f.bridge.mvTexture && !f.bridge.depthTexture && !f.bridge.d3dDevice, "unavailable publication clears obsolete addresses");
        Check(adds == a && descriptions == d, "unavailable resource probe does not retain or describe partial set");
        Check(warnings == w + 1, "unavailable transition is logged once");
        AssertUnchanged(25);
    };
    auto recovered = [&] {
        const auto generation = f.bridge.resourceGeneration;
        RefreshBridgeRenderTargets();
        Check(f.bridge.status == 1 && f.bridge.resourceGeneration == generation + 1, "recovery republishes on the very next available probe");
        AssertUnchanged(10);
    };
    RE::BSGraphics::Renderer::singleton = nullptr;
    unavailable();
    RE::BSGraphics::Renderer::singleton = &f.renderer;
    recovered();
    f.renderer.runtime.renderTargets[0].texture = nullptr;
    unavailable();
    f.renderer.runtime.renderTargets[0].texture = &f.mv;
    recovered();
    f.renderer.runtime.forwarder = nullptr;
    unavailable();
    f.renderer.runtime.forwarder = &f.device;
    recovered();
    f.renderer.runtime.context = nullptr;
    unavailable();
    f.renderer.runtime.context = &f.context;
    recovered();

    f.renderer.depth.depthStencils[0].texture = nullptr;
    RefreshBridgeRenderTargets();
    Check(f.bridge.status == 1 && !f.bridge.depthTexture && !f.bridge.depthSRV, "MV-only readiness remains existing policy and ignores orphan depth view");
    AssertUnchanged(25);
    f.renderer.runtime.renderTargets[0].texture = nullptr;
    f.renderer.depth.depthStencils[0].texture = &f.depth;
    unavailable();
    Check(f.depth.refs == 1, "depth-only set does not become ready");
    f.renderer.runtime.renderTargets[0].texture = &f.mv;
    recovered();
}

void ReaderPinsRetiredSet()
{
    ID3D11Texture2D replacement;
    Fixture f;
    RefreshBridgeRenderTargets();
    auto* oldDepth = reinterpret_cast<ID3D11Texture2D*>(f.bridge.depthTexture);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&f.bridge.resourceReaders));
    f.renderer.depth.depthStencils[0].texture = &replacement;
    std::atomic<bool> completed{false};
    std::thread publisher([&] { RefreshBridgeRenderTargets(); completed.store(true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((ReadBridgeAtomic(f.bridge.publishSequence) & 1) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    Check((ReadBridgeAtomic(f.bridge.publishSequence) & 1) != 0, "writer marks publication unstable while external reader pins old set");
    Check(!completed.load() && oldDepth->refs == 2, "old publication remains retained until reader finishes AddRef");
    oldDepth->AddRef(); // External bridge reader secures its own COM snapshot.
    InterlockedDecrement(reinterpret_cast<volatile LONG*>(&f.bridge.resourceReaders));
    publisher.join();
    Check(completed && !(ReadBridgeAtomic(f.bridge.publishSequence) & 1), "writer completes with stable publication after reader leaves");
    Check(oldDepth->refs == 2 && replacement.refs == 2, "retired texture stays alive through the reader's owned snapshot");
    Check(f.bridge.depthTexture == reinterpret_cast<uint64_t>(&replacement), "new publication exposes replacement after safe retirement");
    oldDepth->Release();
    Check(oldDepth->refs == 1, "retired texture returns to renderer ownership after snapshot release");
    AssertUnchanged(10);
}
}

int main()
{
    InitialAndSteadyState();
    Replacements();
    UnavailableAndRecovery();
    ReaderPinsRetiredSet();
    Check(adds == releases, "all bridge and reader retained references are balanced");
    std::printf("Bridge refresh: %u checks passed; unchanged probes perform no COM/descriptor calls.\n", checks);
}
