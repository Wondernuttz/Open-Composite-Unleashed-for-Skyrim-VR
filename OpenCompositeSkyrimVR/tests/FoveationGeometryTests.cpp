// Runs the unmodified production BeginVRSGameFrame admission/blackout prefix.
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
#include <limits>
#include <vector>
#include "OpenOVR/Compositor/VRSGaze.h"
#include "OpenOVR/Misc/FoveationProfiles.h"
#include "OpenOVR/Misc/FoveationRates.h"
#include "OpenOVR/Misc/FoveationGeometrySettings.h"
#include "OpenOVR/Misc/EffectFoveationState.h"
#include "OpenOVR/Compositor/RDMDiagnosticSchedule.h"
#include "DrvOpenXR/DapaMaskBridge.h"

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
    bool cull = false, middleBlackout = false, outerBlackout = false, cutoff = false, dapa = false;
    float cutoffRadius = 1.f;
    std::string backend = "auto";
    float horizontalScale = 1.f, horizontalOffset = 0.f, verticalOffset = 0.f;
    float VrsEyeHorizontalScale() const { return horizontalScale; }
    float VrsEyeHorizontalOffset() const { return horizontalOffset; }
    float VrsEyeVerticalOffset() const { return verticalOffset; }
    bool VrsAnyEnabled() const { return eye || fixed; }
    bool VrsEyeTracked() const { return eye; }
    bool VrsFixedEnabled() const { return fixed; }
    bool VrsEyeCustomRates() const { return custom; }
    bool VrsEyeCompatibilityMode() const { return cap; }
    bool VrsEyeBlackoutCull() const { return cull; }
    bool VrsEyeAnyBlackout() const { return middleBlackout || outerBlackout || cutoff; }
    bool VrsEyeMiddleBlackout() const { return middleBlackout; }
    bool VrsEyeOuterBlackout() const { return outerBlackout; }
    bool VrsEyePeripheralMask() const { return cutoff; }
    float VrsEyePeripheralMaskRadius(float middle) const {
        ocu_foveation::Blackout mask; mask.cutoffRadius = cutoffRadius;
        return float(ocu_foveation::BlackoutCutoff(mask, middle));
    }
    bool ASWEnabled() const { return dapa; }
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
static RDMDiagnosticSchedule rdmDiagnosticSchedule;
static int OCBridge_MenuState() { return menu ? 1 : 0; }
struct MaskBridge {
    unsigned char _padPreFP[3]{DapaMaskBridge::Format};
    unsigned char preFPDepthCaptured=1;
    std::uint64_t preFPDepthTexture=0;
    std::uint32_t maskAccessGate=0,maskFrameGeneration=0,maskConflictSerial=0,maskFrameConflictBaseline=0;
} bridgeMask;
struct MaskBridgePointer {
    MaskBridge* value;
    MaskBridge* Get() const { return value; }
    MaskBridge* operator->() const { return value; }
    operator MaskBridge*() const { return value; }
};
static MaskBridgePointer s_pBridge{&bridgeMask};
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
static bool s_vrsPatternReady, s_vrsHasSmoothedGaze, sceneArmed, s_vrsFrameArmed, s_vrsHookApplied;
static bool hooksAvailable = true, s_blackoutPresentationFailed = false;
static bool InstallSceneTargetHooks(ID3D11Device*) { return hooksAvailable; }
struct TestEyeRegion { int x = 0, y = 0, width = 96, height = 64; } s_vrsEyeRegion[2];
struct FakeBlackoutRenderer {
    bool available = true;
    unsigned attempts = 0;
    bool Initialize(ID3D11Device* value) { ++attempts; return value && available; }
} s_blackoutRenderer;
static ocu_vrs_gaze::Center s_vrsSmoothedGaze[2];
static std::int64_t s_vrsLastGazeQpc;
static float s_vrsOpticalX[2] = {0.4f, 0.6f}, s_vrsOpticalY[2] = {0.55f, 0.55f};
static float s_vrsProjX[2], s_vrsProjY[2];
static float s_vrsTanL[2] = {-0.8f, -1.2f}, s_vrsTanR[2] = {1.2f, 0.8f};
static float s_vrsTanU[2] = {1.1f, 1.1f}, s_vrsTanD[2] = {-0.9f, -0.9f};
static unsigned disarms, rdmEnds;
static void DisarmSceneVRS() { ++disarms; sceneArmed = false; }
struct FakeVrsManager {
    bool available = true;
    bool IsAvailable() const { return available; }
    bool WasInitializationAttempted() const { return true; }
    bool Initialize(ID3D11Device*) { return available; }
    void Disable() {}
    void SetBlackout(const ocu_foveation::Blackout&) {}
} vrsManager;
struct FakeDensityManager { void EndFrame() { ++rdmEnds; } } densityMaskManager;
struct Observation {
    bool admitted = false, shared = false;
    ID3D11Texture2D* depth = nullptr;
    UINT width = 0, height = 0;
    ocu_vrs_gaze::Mode mode = ocu_vrs_gaze::Mode::Off;
    ocu_foveation::Radii radii{};
    ocu_foveation::RingRates rates{};
    ocu_foveation::BlackoutFrame blackout{};
};
class DX11Compositor {
public:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    Observation observed;
    int testSceneEyeSize[2][2]{{64, 48}, {64, 48}};
    void BeginVRSGameFrame();
};
namespace vr { struct VRTextureBounds_t { float uMin, vMin, uMax, vMax; }; }
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
static void CheckBlackoutPreparation(DX11Compositor& compositor,
    ID3D11Texture2D* left, ID3D11Texture2D* right) {
    // Execute production early returns and the production preparation lambda.
    // Only readiness of external renderer/backend/hook objects is controlled.
    for (const char* backend : {"auto", "vrs", "rdm", "effects"})
    for (bool live : {false, true})
    for (bool fallback : {false, true})
    for (bool enabled : {false, true})
    for (bool rendererReady : {false, true})
    for (unsigned flags = 0; flags < 8; ++flags) {
        auto& config = oovr_global_configuration;
        config = {}; config.eye = true; config.fixed = fallback; config.backend = backend;
        config.cull = enabled; config.middleBlackout = (flags & 1) != 0;
        config.outerBlackout = (flags & 2) != 0; config.cutoff = (flags & 4) != 0;
        config.horizontalScale = 1.5f; config.horizontalOffset = .1f; config.verticalOffset = .05f;
        input.valid = live; s_blackoutRenderer.available = rendererReady;
        const auto oldAttempts = s_blackoutRenderer.attempts;
        SubmitPair(left, right); Run(compositor);
        const bool admitted = (live || fallback) && std::string(backend) != "effects";
        const bool shouldPrepare = admitted && live && enabled && flags != 0;
        Check(compositor.observed.admitted == admitted, "backend/mode admission changed during cull test");
        Check(compositor.observed.blackout.Active() == (shouldPrepare && rendererReady),
            "production cull activation disagrees with scene/gaze/opt-in/mask/renderer gates");
        Check(s_blackoutRenderer.attempts - oldAttempts == unsigned(shouldPrepare),
            "disabled/fixed/effects-only/no-mask path initialized final blackout renderer");
        if (!compositor.observed.blackout.Active()) continue;
        const auto& frame = compositor.observed.blackout;
        Check(frame.mask.middle == config.middleBlackout && frame.mask.outer == config.outerBlackout &&
            frame.mask.cutoff == config.cutoff, "blackout mask combination changed at scene admission");
        Check(frame.inner == .2f && frame.middle == .4f && frame.horizontalScale == 1.5f,
            "blackout did not preserve raw ellipse radii/width");
        Check(frame.mask.guardPixels == 16.f, "DAPA-off path changed ordinary sampling border");
        Check(frame.frameId == Published().frameId, "blackout lost current published frame identity");
        for (int eye = 0; eye < 2; ++eye) {
            Check(frame.centers[eye][0] == s_vrsProjX[eye] && frame.centers[eye][1] == s_vrsProjY[eye],
                "blackout did not use the adjusted per-eye production center");
            Check(frame.sceneEyeSize[eye][0] == compositor.testSceneEyeSize[eye][0] &&
                frame.sceneEyeSize[eye][1] == compositor.testSceneEyeSize[eye][1],
                "blackout did not retain actual scene dimensions for both eyes");
        }
    }

    auto& config = oovr_global_configuration;
    config = {}; config.eye = true; config.cull = true; config.outerBlackout = true;
    input.valid = true; s_blackoutRenderer.available = true;
    for (bool fallback : {false, true}) {
        config.eye = false; config.fixed = fallback;
        const auto oldAttempts = s_blackoutRenderer.attempts;
        SubmitPair(left, right); Run(compositor);
        Check(compositor.observed.admitted == fallback && !compositor.observed.blackout.Active(),
            "saved cull opt-in leaked into fixed-only or fully disabled settings");
        Check(s_blackoutRenderer.attempts == oldAttempts,
            "fixed-only headset path initialized final blackout renderer");
    }
    config.eye = true; config.fixed = true;
    for (unsigned fault = 0; fault < 4; ++fault) {
        menu = fault == 0; hooksAvailable = fault != 1;
        s_blackoutPresentationFailed = fault == 2;
        vrsManager.available = fault != 3; config.backend = "vrs";
        const auto oldAttempts = s_blackoutRenderer.attempts;
        SubmitPair(left, right); Run(compositor);
        Check(!compositor.observed.blackout.Active(), "failed presentation/hook/backend/menu admitted culling");
        Check(s_blackoutRenderer.attempts == oldAttempts, "failed admission reached final renderer initialization");
        menu = false; hooksAvailable = true; s_blackoutPresentationFailed = false;
        vrsManager.available = true;
        SubmitPair(left, right); Run(compositor);
        Check(compositor.observed.blackout.Active(), "next valid frame did not recover cull preparation");
    }
    // Auto can fall back to RDM while explicit VRS cannot use unavailable VRS.
    config.backend = "auto"; vrsManager.available = false;
    SubmitPair(left, right); Run(compositor);
    Check(compositor.observed.blackout.Active(), "hardware-unavailable Auto blocked RDM cull preparation");
    vrsManager.available = true;

    struct Sizes { int scene[2][2], submitted[2][2]; float expectedGuard; };
    const Sizes sizes[] = {
        {{{2048, 2048}, {1536, 1024}}, {{2048, 2048}, {1536, 1024}}, 258.f}, // native
        {{{1024, 512}, {640, 768}}, {{2048, 1024}, {1280, 1536}}, 130.f}, // upscaled output
        {{{2048, 1024}, {1536, 768}}, {{128, 128}, {64, 64}}, 1562.f}, // downscaled output
        {{{400, 200}, {320, 160}}, {{400, 200}, {320, 160}}, 67.f}, // pixel probes dominate
        {{{100, 200}, {300, 400}}, {{1000, 1000}, {1000, 1000}}, 52.f}, // right height dominates
        {{{100, 200}, {300, 400}}, {{1000, 10}, {1000, 1000}}, 1302.f}, // left height dominates
        {{{100, 200}, {300, 400}}, {{5, 1000}, {1000, 1000}}, 1302.f}, // left width dominates
        {{{100, 200}, {300, 400}}, {{1000, 1000}, {10, 1000}}, 1952.f}, // right width dominates
    };
    config.dapa = true;
    for (const auto& size : sizes) {
        for (int eye = 0; eye < 2; ++eye) {
            for (int axis = 0; axis < 2; ++axis)
                compositor.testSceneEyeSize[eye][axis] = size.scene[eye][axis];
            s_vrsEyeRegion[eye].width = size.submitted[eye][0];
            s_vrsEyeRegion[eye].height = size.submitted[eye][1];
        }
        SubmitPair(left, right); Run(compositor);
        const auto& frame = compositor.observed.blackout;
        Check(frame.Active() && frame.mask.guardPixels == size.expectedGuard,
            "production DAPA donor guard is wrong for scene/submitted resolution ratio");
        for (int eye = 0; eye < 2; ++eye) {
            Check(ocu_foveation::BlackoutSupportsDapaSource(frame, eye,
                size.submitted[eye][0], size.submitted[eye][1]),
                "prepared guard does not satisfy actual DAPA source validator");
            for (int axis = 0; axis < 2; ++axis) {
                // Independent source-pixel statement of the solver/probe needs.
                const double retainedPixels = double(frame.mask.guardPixels) *
                    size.submitted[eye][axis] / size.scene[eye][axis];
                Check(retainedPixels > 64.5 &&
                    retainedPixels > .12 * size.submitted[eye][axis] + .5,
                    "donor border omits bounded DAPA search or bilinear support");
            }
        }
    }
    config = {}; input.valid = false;
}

static void CheckSubmittedRegions(ID3D11Device* device, ID3D11DeviceContext* context) {
    struct RegionCase {
        UINT width, height; vr::VRTextureBounds_t bounds;
        bool supplied, valid; UINT left, top, right, bottom;
    };
    const RegionCase cases[] = {
        {32, 16, {}, false, true, 0, 0, 32, 16},
        {32, 16, {0, 0, 1, 1}, true, true, 0, 0, 32, 16}, // explicit full separate eye
        {32, 16, {0, .25f, .5f, .75f}, true, true, 0, 4, 16, 12},
        {32, 16, {.5f, .75f, 1, .25f}, true, true, 16, 4, 32, 12}, // reversed vertical
        {32, 16, {1, .75f, .5f, .25f}, true, true, 16, 4, 32, 12}, // both axes reversed
        {33, 17, {0, 0, .5f, 1}, true, true, 0, 0, 16, 17},
        {33, 17, {.5f, 0, 1, 1}, true, true, 16, 0, 32, 17}, // equal odd-width eye spans
        {33, 17, {.25f, .25f, .75f, .75f}, true, true, 8, 4, 24, 12},
        {32, 16, {-1, -1, .5f, 2}, true, true, 0, 0, 16, 16}, // clamped
        {32, 16, {1, 0, 2, 1}, true, false},
        {32, 16, {0, 1, 1, 2}, true, false},
        {32, 16, {.5f, 0, .5f, 1}, true, false},
        {32, 16, {0, .5f, 1, .5f}, true, false},
        {32, 16, {0, 0, .001f, 1}, true, false},
        {0, 16, {}, false, false}, {32, 0, {}, false, false},
    };
    for (const auto& value : cases) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = value.width; desc.Height = value.height;
        D3D11_BOX region{};
        Check(ResolveSubmittedTextureRegion(desc, value.supplied ? &value.bounds : nullptr, region) == value.valid,
            "submitted texture region validity is wrong");
        if (!value.valid) continue;
        Check(region.left == value.left && region.top == value.top && region.right == value.right &&
            region.bottom == value.bottom && region.front == 0 && region.back == 1,
            "submitted eye crop/axis reversal/odd dimension rectangle is wrong");

        // The exact returned box must produce the expected rows in the same
        // orientation when used by the ordinary source-region copy.
        desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32_UINT;
        std::vector<unsigned> pixels(desc.Width * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            for (UINT x = 0; x < desc.Width; ++x) pixels[y * desc.Width + x] = (y << 16) | x;
        D3D11_SUBRESOURCE_DATA data{pixels.data(), desc.Width * sizeof(unsigned), 0};
        ComPtr<ID3D11Texture2D> source, destination;
        HR(device->CreateTexture2D(&desc, &data, &source));
        desc.Width = value.right - value.left; desc.Height = value.bottom - value.top;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc, nullptr, &destination));
        context->CopySubresourceRegion(destination.Get(), 0, 0, 0, 0, source.Get(), 0, &region);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HR(context->Map(destination.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const unsigned*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x)
                Check(row[x] == ((y + value.top) << 16 | (x + value.left)),
                    "actual source copy changed crop coordinates or flipped an unflipped row");
        }
        context->Unmap(destination.Get(), 0);
    }
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        for (int axis = 0; axis < 4; ++axis) {
            vr::VRTextureBounds_t bounds{0, 0, 1, 1};
            switch (axis) { case 0: bounds.uMin = invalid; break; case 1: bounds.vMin = invalid; break;
                case 2: bounds.uMax = invalid; break; default: bounds.vMax = invalid; break; }
            D3D11_TEXTURE2D_DESC desc{}; desc.Width = 32; desc.Height = 16;
            D3D11_BOX region{};
            Check(!ResolveSubmittedTextureRegion(desc, &bounds, region), "nonfinite submitted bounds accepted");
        }
    }
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
        const float baseLeft=s_vrsProjX[0],baseRight=s_vrsProjX[1],baseY=s_vrsProjY[0];
        oovr_global_configuration.horizontalScale=1.5f;
        oovr_global_configuration.horizontalOffset=.1f;
        oovr_global_configuration.verticalOffset=.05f;
        SubmitPair(left.Get(),right.Get());Run(compositor);
        Check(std::fabs(s_vrsProjX[0]-(baseLeft-.1f))<1.e-5f &&
            std::fabs(s_vrsProjX[1]-(baseRight+.1f))<1.e-5f &&
            std::fabs(s_vrsProjY[0]-(baseY+.05f))<1.e-5f,"actual pre-render centers apply mirrored offsets after gaze smoothing");
        Check(std::fabs(Published().innerRadius-.3f)<1.e-5f &&
            std::fabs(Published().midRadius-.6f)<1.e-5f,"existing effect API encloses wider scene ellipse");
        input.valid = false;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        Check(compositor.observed.admitted && compositor.observed.mode == ocu_vrs_gaze::Mode::Fixed,
            "gaze-loss fallback did not use bridge route");
        Check(!s_vrsHasSmoothedGaze && s_vrsProjX[0] == s_vrsOpticalX[0] &&
            s_vrsProjY[1] == s_vrsOpticalY[1], "fallback retained stale gaze instead of optical centers");
        Check(Published().gazeSampleTime == 0, "fixed fallback published a stale gaze timestamp");
        Check(Published().innerRadius==.7f,"fixed fallback ignores eye width adjustments");
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

        // Effects-only consumers still receive their circular V1 profile even
        // when the user saved a wider scene ellipse. Eye offsets remain useful.
        oovr_global_configuration.eye = true; input.valid = true;
        oovr_global_configuration.horizontalScale = 1.5f;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        const auto effectsBaseline = Published();
        Check(!compositor.observed.admitted && !sceneArmed && bridgeAcquires == acquisitions &&
            effectsBaseline.mode == ocu_effect_foveation::Mode::EyeTracked,
            "tracked effects-only started scene rendering or required depth");
        Check(std::fabs(effectsBaseline.innerRadius - .2f) < 1.e-5f &&
            std::fabs(effectsBaseline.midRadius - .4f) < 1.e-5f,
            "effects-only enlarged circular API radii using the scene width");
        oovr_global_configuration.horizontalOffset = .1f;
        oovr_global_configuration.verticalOffset = .05f;
        SubmitPair(left.Get(), right.Get()); Run(compositor);
        const auto effectsOffset = Published();
        Check(!compositor.observed.admitted && !sceneArmed && bridgeAcquires == acquisitions &&
            effectsOffset.mode == ocu_effect_foveation::Mode::EyeTracked,
            "adjusted tracked effects-only activated a scene backend");
        Check(effectsOffset.innerRadius == effectsBaseline.innerRadius &&
            effectsOffset.midRadius == effectsBaseline.midRadius &&
            effectsOffset.shape == ocu_effect_foveation::Shape::UVRadialHalfExtent,
            "effects-only offsets changed the circular API shape or radii");
        for (int eye = 0; eye < 2; ++eye) {
            Check(std::fabs(effectsOffset.centerUV[eye][0] -
                    (effectsBaseline.centerUV[eye][0] + (eye == 0 ? -.1f : .1f))) < 1.e-5f &&
                std::fabs(effectsOffset.centerUV[eye][1] -
                    (effectsBaseline.centerUV[eye][1] + .05f)) < 1.e-5f,
                "tracked effects-only lost mirrored horizontal or shared vertical offsets");
            Check(effectsOffset.centerUV[eye][0] == s_vrsProjX[eye] &&
                effectsOffset.centerUV[eye][1] == s_vrsProjY[eye],
                "effects-only publication differs from adjusted production centers");
        }
        CheckBlackoutPreparation(compositor, left.Get(), right.Get());
        CheckSubmittedRegions(device.Get(), context.Get());
        std::printf("PASS: %u production admission/blackout/guard/copy checks; real WARP descriptors and cropped pixel readback, no headset initialization\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what()); return 1;
    }
}
