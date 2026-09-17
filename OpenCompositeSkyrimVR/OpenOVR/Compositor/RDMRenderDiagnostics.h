#pragma once
#include "RDMRenderScope.h"
#include "VRSShaderGuard.h"
#include "../logging.h"
#include <sstream>

// Formatting only, called after the frame is disarmed. Extra state capture is
// controlled by Arm's diagnostics flag; this never queries or mutates D3D state.
inline void LogRDMRejectedState(const char* category,
    const RDMRenderScope::RejectedStateDiagnostic& sample)
{
    if (!sample.valid) return;
    OOVR_LOGF("rdm-reject-diag-v2 sample=%s query=%u draw=%u reject=%s start=%s scopeMatched=%u expectedDepthBound=%u expected=%ux%u PS=%u watched=%u cachedReasons=0x%X actualShaderReasons=0x%X outputs=0x%X RTVs=0x%X classInstances=%u shaderCacheMismatch=%u blendCacheMismatch=%u",
        category, sample.queryOrdinal, sample.drawOrdinal,
        RDMRenderScope::QueryRejectName(sample.queryReject),
        RDMRenderScope::StartFailureName(sample.startFailure),
        unsigned(sample.scopeMatched), unsigned(sample.expectedDepthBound),
        sample.expectedWidth, sample.expectedHeight, unsigned(sample.hasPixelShader),
        unsigned(sample.watchedContext), sample.cachedReasons, sample.actualShaderReasons,
        sample.actualColorOutputs, sample.boundRtvMask, sample.classInstances,
        unsigned(sample.shaderCacheMismatch), unsigned(sample.blendCacheMismatch));
    const auto& d = sample.depthState;
    const auto& t = sample.depthTexture;
    const auto& v = sample.depthView;
    OOVR_LOGF("rdm-reject-diag-v2 sample=%s depthState=%u enable=%u compare=%u write=%u stencil=%u stencilRead=0x%X stencilWrite=0x%X ref=%u front=(%u,%u,%u,%u) back=(%u,%u,%u,%u) depthTexture=%u size=%ux%u format=%u bind=0x%X mips=%u array=%u samples=%u DSV=(format=%u dimension=%u flags=0x%X mip=%u) sampleMask=0x%X alphaToCoverage=%u",
        category, unsigned(sample.hasDepthState), unsigned(d.DepthEnable), unsigned(d.DepthFunc),
        unsigned(d.DepthWriteMask), unsigned(d.StencilEnable), unsigned(d.StencilReadMask),
        unsigned(d.StencilWriteMask), sample.stencilReference,
        unsigned(d.FrontFace.StencilFunc), unsigned(d.FrontFace.StencilFailOp),
        unsigned(d.FrontFace.StencilDepthFailOp), unsigned(d.FrontFace.StencilPassOp),
        unsigned(d.BackFace.StencilFunc), unsigned(d.BackFace.StencilFailOp),
        unsigned(d.BackFace.StencilDepthFailOp), unsigned(d.BackFace.StencilPassOp),
        unsigned(sample.hasDepthTexture), t.Width, t.Height, unsigned(t.Format), t.BindFlags,
        t.MipLevels, t.ArraySize, t.SampleDesc.Count, unsigned(v.Format), unsigned(v.ViewDimension),
        v.Flags, v.Texture2D.MipSlice, sample.sampleMask, unsigned(sample.actualAlphaToCoverage));
    const auto& vp = sample.firstViewport;
    const auto& sc = sample.firstScissor;
    const auto& rs = sample.rasterizer;
    OOVR_LOGF("rdm-reject-diag-v2 sample=%s viewportCount=%u firstViewport=(%.3f,%.3f,%.3f,%.3f,%.6f,%.6f) scissorCount=%u firstScissor=(%ld,%ld,%ld,%ld) rasterizer=%u fill=%u cull=%u scissor=%u depthClip=%u depthBias=%d slopeBias=%.6f",
        category, sample.viewportCount, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
        vp.MinDepth, vp.MaxDepth, sample.scissorCount, sc.left, sc.top, sc.right, sc.bottom,
        unsigned(sample.hasRasterizer), unsigned(rs.FillMode), unsigned(rs.CullMode),
        unsigned(rs.ScissorEnable), unsigned(rs.DepthClipEnable), rs.DepthBias, rs.SlopeScaledDepthBias);
    for (unsigned i = 0; i < sample.targets.size(); ++i) {
        const auto& target = sample.targets[i];
        if (!target.bound) continue;
        const auto& td = target.description;
        OOVR_LOGF("rdm-reject-diag-v2 sample=%s RTV=%u texture=%u size=%ux%u format=%u viewFormat=%u viewDimension=%u viewMip=%u bind=0x%X mips=%u array=%u samples=%u declaredOutput=%u blend=%u writeMask=0x%X logicOp=%u",
            category, i, unsigned(target.texture), td.Width, td.Height, unsigned(td.Format),
            unsigned(target.view.Format), unsigned(target.view.ViewDimension), target.view.Texture2D.MipSlice,
            td.BindFlags, td.MipLevels, td.ArraySize, td.SampleDesc.Count,
            unsigned((sample.actualColorOutputs & (1u << i)) != 0),
            unsigned(target.effectiveBlend.BlendEnable), unsigned(target.effectiveBlend.RenderTargetWriteMask),
            unsigned(target.logicOp));
    }
}

inline void LogRDMFrame(RDMRenderScope& scope, bool reportScheduled)
{
    if (!reportScheduled) return;
    RDMRenderScope::GpuSample gpu;
    if (scope.TakeGpuSample(gpu)) {
        OOVR_LOGF("rdm-gpu-v1 completed-older-sample=%llu preparationMs=%.4f resolveMs=%.4f preparationSegments=%u resolveSegments=%u armedFrameSpanMs=%.4f spanIncludesGameWork=1 timestampsMayIncludeIdle=1",
            (unsigned long long)gpu.sampleId, gpu.preparationMs, gpu.resolveMs,
            gpu.preparationSegments, gpu.resolveSegments, gpu.frameSpanMs);
        OOVR_LOGF("rdm-work-v1 completed-older-sample=%llu armedFrameVS=%llu armedFramePS=%llu armedFrameCS=%llu frameIncludesGameWork=1 measuredDrawsAreFirstFourPerStage=1 subsetIsNotTotalGpuCostOrSavings=1 cpuWallIncludesDriverWork=1 firstSampleMayIncludeQueryAllocation=1 queriesMayPerturbWork=1",
            (unsigned long long)gpu.sampleId,
            (unsigned long long)gpu.framePipeline.VSInvocations,
            (unsigned long long)gpu.framePipeline.PSInvocations,
            (unsigned long long)gpu.framePipeline.CSInvocations);
        static constexpr const char* names[] = { "guide-capture", "guide-invalidation", "scene-masked", "scene-full-rate" };
        for (unsigned i = 0; i < gpu.work.size(); ++i) {
            const auto& work = gpu.work[i];
            OOVR_LOGF("rdm-work-v1 completed-older-sample=%llu stage=%s allCalls=%u allCpuWallMs=%.4f measuredCalls=%u measuredGpuSubsetMs=%.4f measuredVS=%llu measuredPS=%llu",
                (unsigned long long)gpu.sampleId, names[i], work.calls, work.cpuMs,
                work.measuredCalls, work.gpuSubsetMs,
                (unsigned long long)work.vsInvocations, (unsigned long long)work.psInvocations);
        }
    }
    const auto stats = scope.Stats();
    if (!stats.draws) return;
    OOVR_LOGF("RDM handoff v2: draws=%u masked=%u protected=%u batches=%u MRTresolves=%u consumerBoundaries=%u stateQueries=%u guideDraws=%u guideInvalidationDraws=%u guideResets=%u noGuideStates=%u resolve=packed-original-targets depth=original-unmodified",
        stats.draws, stats.maskedDraws, stats.protectedDraws, stats.batches,
        stats.resolves, stats.consumerBoundaries, stats.stateQueries,
        stats.guideDraws, stats.guideInvalidationDraws, stats.guideResets, stats.noGuideStates);
    OOVR_LOGF("rdm-cost-v1 sampled-frame: maskPreparations=%u maskReuses=%u guideStateQueries=%u emptyGuideSkips=%u (operation counts, not GPU timings)",
        stats.maskPreparations, stats.maskReuses, stats.guideStateQueries, stats.emptyGuideSkips);
    OOVR_LOGF("rdm-depth-binding-v1 sampled-frame: maskedDraws=%u privateBinds=%u originalRestores=%u (operation counts, not GPU timings)",
        stats.maskedDraws, stats.privateDepthBinds, stats.originalDepthRestores);
    OOVR_LOGF("rdm-guide-cache-v1 sampled-frame: depthReads=%u targetReads=%u hazardReads=%u admissionCpuWallMs=%.4f guideQueryCpuWallMs=%.4f cpuWallIncludesDriverWork=1",
        stats.guideDepthReads, stats.guideTargetReads, stats.guideHazardReads,
        stats.admissionCpuMs, stats.guideQueryCpuMs);
    const auto& diagnostics = scope.Diagnostics();
    if (!diagnostics.enabled) return;
    const auto shaders = ocu_vrs_guard::Counts();
    OOVR_LOGF("rdm-reject-diag-v2 sampled-frame: acceptedQueries=%u startAttempts=%u partialMaskDraws=%u inactiveTargetDraws=%u cachedReasonUnion=0x%X descriptorSamples=%u shaderCacheMismatches=%u blendCacheMismatches=%u lastStart=%s hresultKnown=%u hresult=0x%08X capturedShaders=(compatible=%llu protected=%llu unknown=%llu)",
        stats.queryAccepted, stats.startAttempts, stats.partialMaskDraws,
        stats.inactiveTargetDraws, stats.cachedShaderReasonUnion,
        stats.diagnosticSamples, stats.shaderCacheMismatchSamples, stats.blendCacheMismatchSamples,
        RDMRenderScope::StartFailureName(stats.lastStartFailure), unsigned(stats.lastStartHresultKnown),
        unsigned(stats.lastStartHresult), (unsigned long long)shaders.compatible,
        (unsigned long long)shaders.protectedShaders, (unsigned long long)shaders.unclassified);
    std::ostringstream rejected;
    for (unsigned i = 0; i < RDMRenderScope::QueryRejectCount; ++i) {
        if (stats.queryRejectCounts[i])
            rejected << RDMRenderScope::QueryRejectName(static_cast<RDMRenderScope::QueryReject>(i))
                << '=' << stats.queryRejectCounts[i] << ' ';
    }
    OOVR_LOGF("rdm-reject-diag-v2 first-rejection queries: %s", rejected.str().c_str());
    std::ostringstream draws;
    unsigned accounted = stats.startRejectedDraws;
    for (unsigned i = 0; i < RDMRenderScope::QueryRejectCount; ++i) {
        accounted += stats.rejectedDrawCounts[i];
        if (stats.rejectedDrawCounts[i])
            draws << RDMRenderScope::QueryRejectName(static_cast<RDMRenderScope::QueryReject>(i))
                << '=' << stats.rejectedDrawCounts[i] << ' ';
    }
    OOVR_LOGF("rdm-reject-diag-v2 protected draws: %sstart-failure=%u accounted=%u protected=%u",
        draws.str().c_str(), stats.startRejectedDraws, accounted, stats.protectedDraws);
    std::ostringstream failed;
    for (unsigned i = 0; i < RDMRenderScope::StartFailureCount; ++i) {
        if (stats.startFailureCounts[i])
            failed << RDMRenderScope::StartFailureName(static_cast<RDMRenderScope::StartFailure>(i))
                << '=' << stats.startFailureCounts[i] << ' ';
    }
    OOVR_LOGF("rdm-reject-diag-v2 start failures: %s", failed.str().c_str());
    LogRDMRejectedState("scope", diagnostics.scope);
    LogRDMRejectedState("query", diagnostics.query);
    LogRDMRejectedState("start", diagnostics.start);
    for (unsigned i = 0; i < RDMRenderScope::QueryRejectCount; ++i) {
        if (i == static_cast<unsigned>(RDMRenderScope::QueryReject::SceneScope)) continue;
        LogRDMRejectedState(RDMRenderScope::QueryRejectName(
            static_cast<RDMRenderScope::QueryReject>(i)), diagnostics.byReason[i]);
    }
}
