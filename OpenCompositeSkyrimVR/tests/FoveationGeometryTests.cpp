// Runs the unmodified production BeginVRSGameFrame admission/bridge prefix.
// OpenXR input/bridge publication are controlled shells. Textures/descriptors,
// gaze projection/mode selection, radii/rates and effect publication are real.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include "OpenOVR/Compositor/VRSGaze.h"
#include "OpenOVR/Misc/FoveationProfiles.h"
#include "OpenOVR/Misc/FoveationRates.h"
#include "OpenOVR/Misc/EffectFoveationState.h"

using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Check(bool value, const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
static void HR(HRESULT value) { if (FAILED(value)) throw std::runtime_error("DX11 fixture creation failed"); }
#define OOVR_LOG(...) ((void)0)
#define OOVR_LOGF(...) ((void)0)
#define OOVR_LOG_LIMITEDF(...) ((void)0)
static bool oovr_debug_logging_enabled() { return false; }

struct TestConfiguration {
    bool eye = false, fixed = true, custom = true, cap = false;
    std::string backend = "auto";
    bool VrsAnyEnabled() const { return eye || fixed; }
    bool VrsEyeTracked() const { return eye; }
    bool VrsFixedEnabled() const { return fixed; }
    bool VrsEyeCustomRates() const { return custom; }
    bool VrsEyeCompatibilityMode() const { return cap; }
    std::string FoveatedBackend() const { return backend; }
    ocu_foveation::Radii FoveationRadii(bool tracked) const {
        return ocu_foveation::Resolve(tracked, -1, -1, -1, -1, -1, -1);
    }
    ocu_foveation::RingRates FoveationRates(bool tracked) const {
        return ocu_foveation::ResolveRates(tracked, custom, tracked ? cap : true, true,
            {ocu_foveation::Rate::X1x1, ocu_foveation::Rate::X2x2, ocu_foveation::Rate::X4x2});
    }
} oovr_global_configuration;
static bool menu;
static int OCBridge_MenuState() { return menu ? 1 : 0; }
struct MaskBridge { unsigned preFPDepthCaptured = 1; } bridgeMask;
static MaskBridge* s_pBridge = &bridgeMask;
struct XrGlobal { XrTime nextPredictedFrameTime = 100000; } xrGlobal;
static auto* xr_gbl = &xrGlobal;
struct BaseInput {
    bool valid = false;
    XrVector3f direction{0.25f, -0.15f, -1.0f};
    bool SampleEyeGazeDirection(XrTime, XrVector3f& out, XrPosef*, XrTime& time) {
        out = direction; time = 99999; return valid;
    }
} input;
static bool inputAvailable = true;
static BaseInput* GetUnsafeBaseInput() { return inputAvailable ? &input : nullptr; }

struct OCBridgeResourceSnapshot {
    bool ready = true;
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    ID3D11Texture2D* depthTexture = nullptr;
    bool Ready() const { return ready; }
} bridge;
static bool bridgeAvailable = true;
static unsigned bridgeAcquires;
static bool AcquireBridgeResourceSnapshot(OCBridgeResourceSnapshot& out) {
    ++bridgeAcquires; out = bridge; return bridgeAvailable;
}
static std::uint8_t s_vrsGeometryMask;
static ComPtr<ID3D11Texture2D> s_vrsRenderTexture[2];
static int s_vrsRenderWidth[2], s_vrsRenderHeight[2];
static unsigned s_vrsSceneBindings, s_vrsProtectedBindings, s_vrsUnclassifiedBindings;
static unsigned s_vrsCoarseBindings, s_vrsTerrainDepthBindings, s_vrsGazeDiagnosticCounter;
static ID3D11DeviceContext* s_sceneHookContext;
static ID3D11Texture2D* s_vrsSceneTarget;
static bool s_vrsPatternReady, s_vrsHasSmoothedGaze, sceneArmed;
static ocu_vrs_gaze::Center s_vrsSmoothedGaze[2];
static std::int64_t s_vrsLastGazeQpc;
static float s_vrsOpticalX[2] = {0.4f, 0.6f}, s_vrsOpticalY[2] = {0.55f, 0.55f};
static float s_vrsProjX[2], s_vrsProjY[2];
static float s_vrsTanL[2] = {-0.8f, -1.2f}, s_vrsTanR[2] = {1.2f, 0.8f};
static float s_vrsTanU[2] = {1.1f, 1.1f}, s_vrsTanD[2] = {-0.9f, -0.9f};
static unsigned disarms, rdmEnds;
static void DisarmSceneVRS() { ++disarms; sceneArmed = false; }
struct FakeVrsManager { void Disable() {} } vrsManager;
struct FakeDensityManager { void EndFrame() { ++rdmEnds; } } densityMaskManager;
struct Observation {
    bool admitted = false, shared = false;
    ID3D11Texture2D* depth = nullptr;
    UINT width = 0, height = 0;
    ocu_vrs_gaze::Mode mode = ocu_vrs_gaze::Mode::Off;
    ocu_foveation::Radii radii{};
    ocu_foveation::RingRates rates{};
};
class DX11Compositor {
public:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    Observation observed;
    void BeginVRSGameFrame();
};
#include "FoveationGeometryProduction.inl"

static ocu_effect_foveation::Snapshot Published() {
    ocu_effect_foveation::Snapshot out{};
    Check(ocu_effect_foveation::GetState().Query(ocu_effect_foveation::Version,
        sizeof(out), &out) == ocu_effect_foveation::Result::Success, "effect state query failed");
    return out;
}
static void SubmitPair(ID3D11Texture2D* left, ID3D11Texture2D* right) {
    ID3D11Texture2D* values[2]{left, right};
    for (int eye = 0; eye < 2; ++eye) {
        s_vrsRenderTexture[eye] = values[eye];
        if (values[eye]) {
            D3D11_TEXTURE2D_DESC d{}; values[eye]->GetDesc(&d);
            s_vrsRenderWidth[eye] = int(d.Width); s_vrsRenderHeight[eye] = int(d.Height);
            s_vrsGeometryMask |= std::uint8_t(1u << eye);
        }
    }
}
static void Run(DX11Compositor& compositor) {
    compositor.observed = {};
    sceneArmed = true; bridgeMask.preFPDepthCaptured = 1;
    const auto oldDisarms = disarms, oldEnds = rdmEnds;
    compositor.BeginVRSGameFrame();
    Check(s_vrsGeometryMask == 0, "submission pair was not consumed at frame boundary");
    Check(!sceneArmed && disarms > oldDisarms, "previous scene arm survived boundary");
    Check(rdmEnds > oldEnds, "previous reconstruction was not expired");
    Check(bridgeMask.preFPDepthCaptured == 0, "DAPA mask coverage did not expire");
}
int main() {
    try {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context));
        auto texture = [&](UINT width, UINT height, UINT bind, UINT array = 1) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width; d.Height = height; d.ArraySize = array; d.MipLevels = 1;
            d.Format = bind & D3D11_BIND_DEPTH_STENCIL ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.SampleDesc.Count = 1; d.BindFlags = bind;
            ComPtr<ID3D11Texture2D> t; HR(device->CreateTexture2D(&d, nullptr, &t)); return t;
        };
        auto left = texture(96, 64, D3D11_BIND_RENDER_TARGET);
        auto right = texture(96, 64, D3D11_BIND_RENDER_TARGET);
        auto shared = texture(192, 64, D3D11_BIND_RENDER_TARGET);
        auto depth = texture(128, 48, D3D11_BIND_DEPTH_STENCIL);
        auto oddDepth = texture(127, 48, D3D11_BIND_DEPTH_STENCIL);
        auto arrayDepth = texture(128, 48, D3D11_BIND_DEPTH_STENCIL, 2);
        DX11Compositor compositor{device.Get(), context.Get()};
        bridge = {true, device.Get(), context.Get(), depth.Get()};

        // Fixed-only must reach the bridge route for both scene backends.
        for (const char* backend : {"auto", "vrs", "rdm"}) {
            oovr_global_configuration = {}; oovr_global_configuration.backend = backend;
            SubmitPair(left.Get(), right.Get()); Run(compositor);
            Check(compositor.observed.admitted && !compositor.observed.shared,
                "fixed-only separate eyes did not reach scene backend");
            Check(compositor.observed.depth == depth.Get() && compositor.observed.width == 128 &&
                compositor.observed.height == 48, "output eye dimensions incorrectly became scene atlas");
            Check(compositor.observed.mode == ocu_vrs_gaze::Mode::Fixed &&
                compositor.observed.radii.inner == 0.70f, "fixed route selected moving-eye profile");
            Check(s_vrsProjX[0] == 0.4f && s_vrsProjX[1] == 0.6f,
                "fixed route lost asymmetric optical centers");
        }
        // Gaze can appear, disappear and return without a runtime restart.
        oovr_global_configuration.eye = true; input.valid = true;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(compositor.observed.admitted && compositor.observed.mode == ocu_vrs_gaze::Mode::EyeTracked,
            "moving separate-eye route regressed");
        Check(s_vrsProjX[0] != s_vrsOpticalX[0], "test gaze did not move from optical center");
        input.valid = false;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(compositor.observed.admitted && compositor.observed.mode == ocu_vrs_gaze::Mode::Fixed,
            "gaze-loss fallback did not use bridge route");
        Check(!s_vrsHasSmoothedGaze && s_vrsProjX[0] == s_vrsOpticalX[0] &&
            s_vrsProjY[1] == s_vrsOpticalY[1], "fallback retained stale gaze instead of optical centers");
        Check(Published().gazeSampleTime == 0, "fixed fallback published a stale gaze timestamp");
        input.valid = true;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(compositor.observed.mode == ocu_vrs_gaze::Mode::EyeTracked, "fresh gaze did not recover immediately");
        oovr_global_configuration.fixed = false; input.valid = false;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(!compositor.observed.admitted && Published().mode == ocu_effect_foveation::Mode::Disabled,
            "gaze-loss enabled fixed without permission");
        oovr_global_configuration = {};

        // Old retained texture references cannot form a new stereo pair.
        for (std::uint8_t mask : {std::uint8_t(0), std::uint8_t(1), std::uint8_t(2)}) {
            s_vrsGeometryMask = mask; Run(compositor);
            Check(!compositor.observed.admitted && Published().mode == ocu_effect_foveation::Mode::Disabled,
                "incomplete/stale eye pair admitted");
        }
        s_vrsGeometryMask = 3; s_vrsRenderTexture[1].Reset(); Run(compositor);
        Check(!compositor.observed.admitted, "null submitted eye admitted");

        // Shared target stays available without any bridge snapshot.
        bridgeAvailable = false; auto acquisitions = bridgeAcquires;
        SubmitPair(shared.Get(), shared.Get()); Run(compositor);
        Check(compositor.observed.admitted && compositor.observed.shared && bridgeAcquires == acquisitions,
            "shared fixed path became dependent on bridge");
        bridgeAvailable = true;

        // Every unsafe bridge condition is frame-local and self-recovers.
        for (int fault = 0; fault < 8; ++fault) {
            bridge = {true, device.Get(), context.Get(), depth.Get()}; bridgeAvailable = true;
            switch (fault) {
            case 0: bridgeAvailable = false; break;
            case 1: bridge.ready = false; break;
            case 2: bridge.d3dDevice = nullptr; break;
            case 3: bridge.d3dContext = nullptr; break;
            case 4: bridge.depthTexture = nullptr; break;
            case 5: bridge.depthTexture = oddDepth.Get(); break;
            case 6: bridge.depthTexture = arrayDepth.Get(); break;
            case 7: bridge.depthTexture = shared.Get(); break;
            }
            SubmitPair(left.Get(), right.Get()); Run(compositor);
            Check(!compositor.observed.admitted, "unsafe bridge accepted as scene atlas");
            bridge = {true, device.Get(), context.Get(), depth.Get()}; bridgeAvailable = true;
            SubmitPair(left.Get(), right.Get()); Run(compositor);
            Check(compositor.observed.admitted && compositor.observed.depth == depth.Get(),
                "valid next-frame bridge did not recover");
        }
        // Menu/disabled/effects-only behavior remains independent of scene routing.
        menu = true; SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(!compositor.observed.admitted && Published().mode == ocu_effect_foveation::Mode::Disabled,
            "menu admitted foveation");
        menu = false; oovr_global_configuration.fixed = false;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(!compositor.observed.admitted && Published().mode == ocu_effect_foveation::Mode::Disabled,
            "disabled settings admitted foveation");
        oovr_global_configuration.fixed = true; oovr_global_configuration.backend = "effects";
        acquisitions = bridgeAcquires;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(!compositor.observed.admitted && Published().mode == ocu_effect_foveation::Mode::Fixed &&
            bridgeAcquires == acquisitions, "effects-only started scene backend or required depth");
        std::printf("PASS: %u production foveation admission/bridge checks; real WARP descriptors, no headset initialization\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what()); return 1;
    }
}
