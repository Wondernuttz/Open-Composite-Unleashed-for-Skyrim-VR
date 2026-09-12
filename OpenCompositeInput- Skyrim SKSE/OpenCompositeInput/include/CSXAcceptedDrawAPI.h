#pragma once
// PROPOSED v1 ABI for agreement with CSX. Not an assertion about an existing API.
// Windows x64, default MSVC packing, C-style function table. No STL across ABI.
#include <cstdint>
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace CSXAcceptedDrawAPI {
inline constexpr char ExportName[] = "CSX_GetAcceptedDrawAPI";
inline constexpr uint32_t Version = 1;
enum Result : uint32_t { Success=0, Unsupported=1, NotReady=2, InvalidArgument=3, Failed=4 };
enum Capability : uint64_t {
    FullSceneIndexedCoverage=1ull<<0, // All accepted main-scene indexed/instanced lighting/effect draws.
    NativeVRGeometry=1ull<<1,        // Skyrim VR 1.4.15 BSGeometry, live during callback.
    PostDrawLiveState=1ull<<2,       // After native submission, before bound state changes.
    IsolatedReplay=1ull<<3,          // No observer recursion, capture or normal draw accounting.
    QuiescentUnregister=1ull<<4,
    PackedStereoDepth=1ull<<5       // Current OCU: single-sample, single 2D stereo depth texture.
};
inline constexpr uint64_t RequiredCapabilities = FullSceneIndexedCoverage|NativeVRGeometry|
    PostDrawLiveState|IsolatedReplay|QuiescentUnregister|PackedStereoDepth;
enum DrawKind : uint32_t { Indexed=1, IndexedInstanced=2 };
enum PassKind : uint32_t { MainScene=1, Shadow=2, Reflection=3, UICapture=4, Other=5 };
enum StereoLayout : uint32_t { PackedStereo2D=1 };
struct Arguments {
    uint32_t kind, indexCount, instanceCount, startIndex;
    int32_t baseVertex;
    uint32_t startInstance;
};
// Only valid synchronously inside the originating callback, at most once.
// Reuses current geometry/rasterization and caller's temporary mask output state.
using ReplayFn = uint32_t(__cdecl*)(void* token);
struct Draw {
    uint32_t structSize, version;
    uint64_t drawId; // Strictly increasing, nonzero, process lifetime; one rendering thread.
    ID3D11DeviceContext* context;
    const void* geometry; // Borrowed const RE::BSGeometry*, not a retained ID/pointer.
    ID3D11Texture2D* sceneDepth; // Canonical scene depth; not shadow/menu capture depth.
    uint32_t pass, stereoLayout;
    Arguments arguments;
    ReplayFn replay;
    void* replayToken; // Opaque; null is allowed if provider supports it.
};
using ObserverFn = void(__cdecl*)(const Draw* draw, void* user);
using RegisterFn = uint32_t(__cdecl*)(ObserverFn observer, void* user, uint64_t* subscription);
using UnregisterFn = uint32_t(__cdecl*)(uint64_t subscription);
struct API {
    uint32_t structSize, version;
    uint64_t capabilities;
    RegisterFn registerObserver;
    UnregisterFn unregisterObserver;
};
// Return null for an unsupported version/consumer minimum size. Borrowed table
// and containing module stay alive until all subscriptions are unregistered.
using QueryFn = const API*(__cdecl*)(uint32_t version, uint32_t minimumTableSize);
static_assert(sizeof(void*)==8 && sizeof(Arguments)==24 && sizeof(Draw)==88 && sizeof(API)==32);
}
