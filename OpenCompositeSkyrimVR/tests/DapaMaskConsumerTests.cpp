#include "DrvOpenXR/DapaMaskBridge.h"
#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

struct OCRenderTargetBridge {
    uint64_t preFPDepthTexture = 0;
    uint8_t preFPDepthCaptured = 0;
    uint8_t _padPreFP[3]{DapaMaskBridge::Format, 0, 0};
    uint32_t maskAccessGate = 0, maskFrameGeneration = 0;
    uint32_t maskConflictSerial = 0, maskFrameConflictBaseline = 0;
};
OCRenderTargetBridge bridge;
struct BridgePointer {
    OCRenderTargetBridge* Get() { return &bridge; }
    OCRenderTargetBridge* operator->() { return &bridge; }
} s_pBridge;
uint32_t s_dapaMaskCacheFrame = 0, s_dapaMaskCacheConflicts = 0;
unsigned checks = 0, comCalls = 0;
bool requireLease = true;
void Check(bool result, const char* message)
{
    ++checks;
    if (!result) throw std::runtime_error(message);
}
void CheckCOMLease()
{
    ++comCalls;
    if (requireLease && !bridge.maskAccessGate)
        throw std::runtime_error("COM method or texture copy ran outside the producer/consumer lease");
}
#define OOVR_LOGF(...) ((void)0)
#define OOVR_LOG_LIMITEDF(...) ((void)0)

constexpr unsigned DXGI_FORMAT_R32_FLOAT = 41, D3D11_BIND_SHADER_RESOURCE = 8, D3D11_USAGE_DEFAULT = 0;
using HRESULT = long;
#define SUCCEEDED(hr) ((hr) >= 0)
struct D3D11_TEXTURE2D_DESC {
    unsigned Width = 640, Height = 180, MipLevels = 1, ArraySize = 1, Format = DXGI_FORMAT_R32_FLOAT;
    struct { unsigned Count = 1, Quality = 0; } SampleDesc;
    unsigned Usage = 0, BindFlags = 0, CPUAccessFlags = 0, MiscFlags = 0;
};
struct D3D11_BOX { unsigned left = 0, top = 0, front = 0, right = 320, bottom = 180, back = 1; };
struct ID3D11Device;
struct ID3D11Texture2D {
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Device* owner = nullptr;
    void GetDesc(D3D11_TEXTURE2D_DESC* result) { CheckCOMLease(); *result = desc; }
    void GetDevice(ID3D11Device** result) { CheckCOMLease(); *result = owner; }
};
struct ID3D11ShaderResourceView { };
namespace Microsoft::WRL {
template<class T> struct ComPtr {
    T* value = nullptr;
    T* Get() const { return value; }
    T** operator&() { return &value; }
    T* operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    void Reset() { value = nullptr; }
};
}
struct ID3D11Device {
    bool failTexture = false, failView = false;
    unsigned textureCalls = 0, viewCalls = 0;
    std::vector<std::unique_ptr<ID3D11Texture2D>> textures;
    std::vector<std::unique_ptr<ID3D11ShaderResourceView>> views;
    HRESULT CreateTexture2D(const D3D11_TEXTURE2D_DESC* desc, void*, ID3D11Texture2D** result)
    {
        CheckCOMLease(); ++textureCalls;
        if (failTexture) return -1;
        textures.push_back(std::make_unique<ID3D11Texture2D>(ID3D11Texture2D{*desc, this}));
        *result = textures.back().get(); return 0;
    }
    HRESULT CreateShaderResourceView(ID3D11Texture2D*, void*, ID3D11ShaderResourceView** result)
    {
        CheckCOMLease(); ++viewCalls;
        if (failView) return -1;
        views.push_back(std::make_unique<ID3D11ShaderResourceView>());
        *result = views.back().get(); return 0;
    }
} device, wrongDevice;
struct ID3D11DeviceContext {
    bool failCopy = false;
    unsigned copies = 0;
    std::function<void()> duringCopy;
} contextState;
bool SafeBridgeCopy(ID3D11DeviceContext* context, ID3D11Texture2D* destination,
    unsigned, unsigned, unsigned, unsigned, ID3D11Texture2D* source, unsigned, const D3D11_BOX* region)
{
    CheckCOMLease(); ++context->copies;
    if (context->duringCopy) {
        auto callback = std::move(context->duringCopy);
        callback();
    }
    return destination && source && region && !context->failCopy;
}
namespace ocu_effect_foveation {
struct State { int ReadBlackout() { return 0; } };
State GetState() { return {}; }
}
struct Layer { int pose = 0, fov = 0; } layer;
float g_fsr3CameraNear = 0.1f, g_fsr3CameraFar = 1000;

struct ASWFixture {
    ID3D11Device* m_device = &device;
    unsigned m_depthWidth = 320, m_depthHeight = 180;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bodyDepth[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_bodySrv[2];
    bool m_bodyValid[2]{};
    ID3D11Texture2D* receivedMasks[2]{};
    unsigned eyeMask = 0, cacheCalls = 0, invalidations = 0;
    bool hasPair = false;
    bool IsReady() const { return true; }
    bool HasCachedFrame() const { return hasPair; }
    void InvalidateCachedFrame() { ++invalidations; hasPair = false; eyeMask = 0; }
    bool CopyBody(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* bodyDepthMask,
        ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion)
    {
        auto failGeneration = [this]() { InvalidateCachedFrame(); return false; };
#include "DapaBodyMaskCopyProduction.inc"
        return true;
    }
    bool CacheFrame(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D*, const D3D11_BOX*, bool,
        ID3D11Texture2D*, const D3D11_BOX*, ID3D11Texture2D* depth, const D3D11_BOX* region,
        int, int, float, float, ID3D11Texture2D* mask, int)
    {
        ++cacheCalls;
        CheckCOMLease();
        if (eye == 0) { hasPair = false; eyeMask = 0; }
        else if (eyeMask != 1) { InvalidateCachedFrame(); return false; }
        receivedMasks[eye] = mask;
        if (!CopyBody(eye, ctx, mask, depth, region)) return false;
        eyeMask |= 1u << eye;
        if (eye == 1) { hasPair = eyeMask == 3; eyeMask = 0; }
        return true;
    }
} provider;
ASWFixture* g_aswProvider = &provider;
ID3D11Texture2D sourceDepth{{}, &device}, sourceMask{{}, &device}, colorTexture{{}, &device};

bool Consume(int eyeIdx, bool haveDepth = true, bool validRegion = true)
{
    auto* context = &contextState;
    auto* colorSrc = &colorTexture;
    auto* mvTex = &sourceDepth;
    auto* aswDepthSrc = haveDepth ? &sourceDepth : nullptr;
    D3D11_BOX colorRegion, mvRegion, depthRegion;
    depthRegion.left = eyeIdx * 320; depthRegion.right = depthRegion.left + 320;
    if (!validRegion) colorRegion.right = colorRegion.left;
    bool colorFlipV = false;
#include "DapaMaskConsumerProduction.inc"
    return aswEyeCached;
}
#include "DapaMaskCacheValidProduction.inc"

void Reset()
{
    bridge = {};
    s_dapaMaskCacheFrame = s_dapaMaskCacheConflicts = 0;
    provider = {};
    device.failTexture = device.failView = false;
    contextState = {};
    sourceMask = {{}, &device}; sourceDepth = {{}, &device};
}
void Start(bool ownedMask = true)
{
    if (!DapaMaskBridge::BeginFrame(&bridge)) throw std::runtime_error("fixture could not begin clean frame");
    if (ownedMask) {
        DapaMaskBridge::Access<OCRenderTargetBridge> write(&bridge, DapaMaskBridge::AccessMode::Write);
        if (!write.Publish(reinterpret_cast<uint64_t>(&sourceMask)))
            throw std::runtime_error("fixture could not publish mask");
    }
}
void Pair()
{
    Check(Consume(0), "left eye cached");
    Check(Consume(1) && provider.HasCachedFrame(), "right eye completes same real-frame pair");
    Check(OCBridge_DapaMaskCacheValid(), "complete clean pair remains eligible at injection");
}

int main() try {
    Reset(); Start(); Pair();
    Check(provider.receivedMasks[0] == &sourceMask && provider.receivedMasks[1] == &sourceMask,
        "both eyes receive protected format-2 mask");
    Check(provider.m_bodyValid[0] && provider.m_bodyValid[1], "actual ASW mask-copy block records both eyes");
    Check(bridge.maskAccessGate == 0 && comCalls > 0, "consumer releases lease after COM and copy work");
    Start(false); Pair();
    Check(!provider.receivedMasks[0] && !provider.receivedMasks[1] && !provider.m_bodyValid[0] && !provider.m_bodyValid[1],
        "legitimate no-player frame never reuses preceding player mask");

    Reset(); Start();
    {
        DapaMaskBridge::Access<OCRenderTargetBridge> writer(&bridge, DapaMaskBridge::AccessMode::Write);
        unsigned calls = provider.cacheCalls;
        Check(!Consume(0) && provider.cacheCalls == calls && !provider.hasPair,
            "held producer prevents COM/cache access and invalidates generation without waiting");
    }
    Start(); Pair();
    {
        DapaMaskBridge::Access<OCRenderTargetBridge> reader(&bridge, DapaMaskBridge::AccessMode::Read);
        unsigned calls = provider.cacheCalls;
        Check(!OCBridge_DapaMaskCacheValid(), "injection recheck never waits on a competing mask reader");
        Check(!Consume(0) && provider.cacheCalls == calls && !provider.hasPair,
            "held competing reader makes consumer skip rather than race");
    }
    Start(); Pair();

    Start();
    contextState.duringCopy = [] {
        bool acquired = true;
        std::thread producer([&] {
            DapaMaskBridge::Access<OCRenderTargetBridge> write(&bridge, DapaMaskBridge::AccessMode::Write);
            acquired = bool(write);
        });
        producer.join();
        if (acquired) throw std::runtime_error("producer acquired during a mask copy");
    };
    Check(!Consume(0) && !provider.hasPair, "missed producer draw during copy invalidates the left eye");
    Check(!Consume(1), "poisoned left generation cannot later complete");
    Start(); Pair();

    Start(); Check(Consume(0), "left eye cached before a right-eye copy conflict");
    contextState.duringCopy = [] {
        DapaMaskBridge::Access<OCRenderTargetBridge> write(&bridge, DapaMaskBridge::AccessMode::Write);
        if (write) throw std::runtime_error("producer acquired during right-eye copy");
    };
    Check(!Consume(1) && !provider.hasPair,
        "conflict while right eye publishes a pair revokes provider's completed cache");
    Start(); Pair();

    Start(); Check(Consume(0), "left eye available before generation change");
    Start(); unsigned calls = provider.cacheCalls;
    Check(!Consume(1) && provider.cacheCalls == calls && !provider.hasPair,
        "generation changes between eyes reject before touching cached textures");
    Start(); Pair();

    { DapaMaskBridge::Access<OCRenderTargetBridge> writer(&bridge, DapaMaskBridge::AccessMode::Write); writer.Poison(); }
    Check(!OCBridge_DapaMaskCacheValid(), "poison after right-eye copy blocks synthetic injection");
    Start(); Pair();

    Reset(); bridge._padPreFP[0] = 1; bridge.preFPDepthTexture = 0xDEADBEEF; bridge.preFPDepthCaptured = 1;
    Check(!Consume(0) && !Consume(1) && provider.cacheCalls == 0,
        "legacy marker 1 never dereferences unleased raw texture or silently generates unmasked frames");
    Check(!OCBridge_DapaMaskCacheValid(), "legacy protocol never passes injection recheck");

    Reset(); Start(); Pair();
    Check(!Consume(0, false) && !provider.hasPair, "missing extracted depth invalidates cached pair");
    Start(); Check(!Consume(0, true, false), "invalid submitted color bounds prevent caching");

    Reset(); Start(); sourceMask.desc.Format = 28;
    Check(!Consume(0) && !provider.hasPair, "present non-R32_FLOAT player mask rejects generation");
    sourceMask.desc.Format = DXGI_FORMAT_R32_FLOAT; Start(); Pair();
    sourceMask.owner = &wrongDevice; Start();
    Check(!Consume(0), "different-device mask rejects generation");
    sourceMask.owner = &device; Start(); Pair();
    sourceMask.desc.Width = 320; Start();
    Check(!Consume(0), "source mask resolution mismatch rejects generation");
    sourceMask.desc.Width = 640; sourceMask.desc.ArraySize = 2; Start();
    Check(!Consume(0), "array player mask rejects generation");
    sourceMask.desc.ArraySize = 1; sourceMask.desc.SampleDesc.Count = 2; Start();
    Check(!Consume(0), "multisampled player mask rejects generation");
    sourceMask.desc.SampleDesc.Count = 1; Start(); Pair();

    Reset(); Start(); device.failTexture = true;
    Check(!Consume(0), "mask allocation failure rejects generation");
    device.failTexture = false; Start(); Pair();
    contextState.failCopy = true; Start();
    Check(!Consume(0), "mask copy failure rejects generation");
    contextState.failCopy = false; Start(); Pair();

    Reset(); Start(); device.failView = true;
    Check(!Consume(0) && provider.m_bodyDepth[0] && !provider.m_bodySrv[0], "partial mask allocation fails safely");
    device.failView = false; Start(); Pair();
    Check(provider.m_bodyValid[0], "transient SRV failure retries next frame even when cached texture size already matches");
    std::printf("PASS: %u extracted DAPA consumer/copy checks, %u COM/copy calls under lease\n", checks, comCalls);
    return 0;
} catch (const std::exception& ex) {
    std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, ex.what()); return 1;
}
