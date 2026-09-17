#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <memory>
#include "DensityMaskManager.h"

// A color-pass transaction. The game's depth/stencil resource is never masked.
// Pending sparse MRTs are resolved before another pass can consume them.
class RDMRenderScope {
public:
    RDMRenderScope();
    ~RDMRenderScope();
    RDMRenderScope(const RDMRenderScope&) = delete;
    RDMRenderScope& operator=(const RDMRenderScope&) = delete;
    // Prepare both native protection modes before shader-mod draw observers.
    // Changes only private probe contexts; the supplied device is read-only.
    static bool PrepareDrawHooks(ID3D11Device* device);
    bool Arm(ID3D11DeviceContext* context, ID3D11Texture2D* depth,
        ID3D11Texture2D* submitted, int width, int height,
        const DensityMaskManager::EyeRegion& left, const DensityMaskManager::EyeRegion& right,
        const DensityMaskManager::PatternSettings& pattern, const float centers[4],
        bool diagnosticsEnabled = false);
    void EndFrame();
    void Shutdown();
    enum GuideChanges : unsigned {
        GuideNone = 0, GuideDepth = 1, GuideTargets = 2, GuideBlend = 4,
        GuidePredicate = 8, GuideStreamOutput = 16, GuideUavs = 32,
        GuideShader = 64, GuideAll = 127
    };
    void StateChanged(ID3D11DeviceContext* context, unsigned guideChanges = GuideAll);
    // Restore the game's physical depth binding before replacing output state.
    // Pending color reconstruction remains in the same batch.
    void BeforeDepthStateBoundary();
    static void NotifyBeforeDepthStateBoundary(ID3D11DeviceContext* context);
    // OM getters expose the game's logical DSV without disturbing a compatible
    // draw batch. The native getter's returned reference is replaced in place.
    void ExposeOriginalDepthBinding(ID3D11DepthStencilView** depth);
    static void NotifyState(ID3D11DeviceContext* context, unsigned guideChanges);
    static void NotifyTargets(ID3D11DeviceContext* context);
    static void RegisterTargetObserver(void* renderTargets, void* renderTargetsAndUavs);

    // Optional VRS draw observer shares these native D3D detours. It is never
    // called for RDM rendering, including RDM's internal replay/resolve draws.
    // The immediate-context owner serializes registration and callbacks.
    struct DrawObserver {
        void* owner = nullptr;
        void (*beforeDraw)(void*, ID3D11DeviceContext*) = nullptr;
        void (*afterDraw)(void*, ID3D11DeviceContext*) = nullptr;
        void (*stateChanged)(void*, ID3D11DeviceContext*) = nullptr;
        void (*depthCleared)(void*, ID3D11DeviceContext*, ID3D11DepthStencilView*) = nullptr;
    };
    static bool RegisterDrawObserver(ID3D11DeviceContext* context, const DrawObserver* observer);
    static void RemoveDrawObserver(ID3D11DeviceContext* context);

    // Counts describe first-failing state queries, not rejected draw totals.
    // Cached eligibility can reject several draws after one state query.
    enum class QueryReject : unsigned {
        SceneScope, ShaderReasons, NoDepthGuide, NoDepthState, DepthDisabled,
        DepthComparison, Stencil, SampleMask, ShaderOutputs, StreamOutput,
        AlphaToCoverage, LogicOp, Blend, WriteMask, Predicate, DepthResource,
        DepthTexture, ColorResource, ColorTexture, OmUav, Rasterizer, Viewport, Count
    };
    enum class StartFailure : unsigned {
        DepthFormat, DepthSrv, PrivateDepthTexture, PrivateDsv,
        CoverageTexture, CoverageRtv, CoverageSrv, EligibilityTexture,
        EligibilitySrv, EligibilityUav, EligibilityCompile, EligibilityShader,
        EligibilityBuffer, ResolverInitialize, ResolverTargets, EligibilityMap,
        ApplyMask, Count
    };
    static constexpr unsigned QueryRejectCount = static_cast<unsigned>(QueryReject::Count);
    static constexpr unsigned StartFailureCount = static_cast<unsigned>(StartFailure::Count);
    static const char* QueryRejectName(QueryReject reason) noexcept;
    static const char* StartFailureName(StartFailure stage) noexcept;

    struct Statistics {
        unsigned draws = 0, maskedDraws = 0, batches = 0, resolves = 0;
        unsigned maskPreparations = 0, maskReuses = 0;
        unsigned privateDepthBinds = 0, originalDepthRestores = 0;
        unsigned partialMaskDraws = 0, inactiveTargetDraws = 0;
        unsigned protectedDraws = 0, consumerBoundaries = 0, stateQueries = 0;
        unsigned guideDraws = 0, guideInvalidationDraws = 0, guideResets = 0;
        unsigned guideStateQueries = 0, emptyGuideSkips = 0;
        unsigned guideDepthReads = 0, guideTargetReads = 0, guideHazardReads = 0;
        double admissionCpuMs = 0, guideQueryCpuMs = 0;
        unsigned noGuideStates = 0;
        unsigned queryAccepted = 0, startAttempts = 0;
        std::array<unsigned, QueryRejectCount> queryRejectCounts{};
        std::array<unsigned, QueryRejectCount> rejectedDrawCounts{};
        std::array<unsigned, StartFailureCount> startFailureCounts{};
        unsigned startRejectedDraws = 0;
        unsigned cachedShaderReasonUnion = 0;
        unsigned diagnosticSamples = 0, shaderCacheMismatchSamples = 0;
        unsigned blendCacheMismatchSamples = 0;
        StartFailure lastStartFailure = StartFailure::Count;
        HRESULT lastStartHresult = S_OK;
        bool lastStartHresultKnown = false;
    };
    Statistics Stats() const;
    struct GpuSample {
        std::uint64_t sampleId = 0;
        unsigned preparationSegments = 0, resolveSegments = 0;
        double preparationMs = 0, resolveMs = 0, frameSpanMs = 0;
        struct WorkSample {
            unsigned calls = 0, measuredCalls = 0;
            double cpuMs = 0, gpuSubsetMs = 0;
            std::uint64_t vsInvocations = 0, psInvocations = 0;
        };
        std::array<WorkSample, 4> work{};
        D3D11_QUERY_DATA_PIPELINE_STATISTICS framePipeline{};
    };
    // A completed older sample, delivered once. The frame span includes game
    // work between RDM passes; only the named segments isolate RDM commands.
    bool TakeGpuSample(GpuSample& result);

    struct TargetDiagnostic {
        bool bound = false, texture = false;
        D3D11_TEXTURE2D_DESC description{};
        D3D11_RENDER_TARGET_VIEW_DESC view{};
        D3D11_RENDER_TARGET_BLEND_DESC effectiveBlend{};
        bool logicOp = false;
    };
    struct RejectedStateDiagnostic {
        bool valid = false, scopeMatched = false, expectedDepthBound = false;
        bool hasPixelShader = false, hasDepthState = false, hasDepthTexture = false;
        bool watchedContext = false, shaderCacheMismatch = false, blendCacheMismatch = false;
        unsigned queryOrdinal = 0, drawOrdinal = 0, selectionScore = 0;
        QueryReject queryReject = QueryReject::Count;
        StartFailure startFailure = StartFailure::Count;
        unsigned cachedReasons = 0, actualShaderReasons = 0, actualColorOutputs = 0;
        unsigned classInstances = 0, boundRtvMask = 0, sampleMask = 0, stencilReference = 0;
        unsigned expectedWidth = 0, expectedHeight = 0;
        bool actualAlphaToCoverage = false;
        D3D11_DEPTH_STENCIL_DESC depthState{};
        D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
        D3D11_TEXTURE2D_DESC depthTexture{};
        std::array<TargetDiagnostic, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        unsigned viewportCount = 0, scissorCount = 0;
        D3D11_VIEWPORT firstViewport{};
        D3D11_RECT firstScissor{};
        bool hasRasterizer = false;
        D3D11_RASTERIZER_DESC rasterizer{};
    };
    struct DiagnosticSnapshot {
        bool enabled = false;
        // Separate samples prevent an early shadow/depth-only rejection from
        // displacing a main-scene color rejection or a failed batch setup.
        RejectedStateDiagnostic scope, query, start;
        // Retain the already captured state for each rejection category. A
        // later full-screen pass must not erase the earlier MRT failure.
        std::array<RejectedStateDiagnostic, QueryRejectCount> byReason{};
    };
    // No COM objects or GPU readbacks are retained; read only on log cadence.
    const DiagnosticSnapshot& Diagnostics() const;

    // Entry points shared by the D3D11 detours and the compositor's existing
    // render-target observer. All internal rendering is reentrancy guarded.
    static RDMRenderScope* Active(ID3D11DeviceContext* context);
    bool BeforeDraw();
    void AfterDraw(bool masked);
    void BeforeRead(ID3D11Resource* resource);
    void BeforeWrite(ID3D11Resource* resource);
    bool BeginDepthGuide(bool invalidate);
    void EndDepthGuide();
    void BeginSceneDraw(bool masked);
    void EndSceneDraw(bool masked);
    void InvalidateDepthGuide();
    void BeforeReads(UINT count, ID3D11ShaderResourceView* const* views);
    void BeforeCompute();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
