#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <optional>

namespace ocu_rdm {
// Sample only GPU commands submitted between each pair of timestamps. The
// frame span includes intervening game work and must not be called RDM cost.
class GpuTiming {
public:
    enum class Stage : unsigned { Preparation, Resolve };
    enum class WorkStage : unsigned { GuideCapture, GuideInvalidation, SceneMasked, SceneFullRate };
    static constexpr unsigned WorkStageCount = 4;
    static constexpr unsigned WorkSegmentsPerStage = 4;
    static constexpr unsigned SegmentLimit = 32;
    static constexpr unsigned SlotCount = 3;
    static constexpr unsigned PollDelay = 3;
    struct WorkResult {
        unsigned calls = 0, measuredCalls = 0;
        double cpuMs = 0, gpuSubsetMs = 0;
        std::uint64_t vsInvocations = 0, psInvocations = 0;
    };
    struct Result {
        std::uint64_t sampleId = 0;
        unsigned preparationSegments = 0, resolveSegments = 0;
        double preparationMs = 0, resolveMs = 0, frameSpanMs = 0;
        std::array<WorkResult, WorkStageCount> work{};
        D3D11_QUERY_DATA_PIPELINE_STATISTICS framePipeline{};
    };
    struct Counters {
        std::uint64_t queriesCreated = 0, commandsIssued = 0, getDataCalls = 0;
        std::uint64_t samplesStarted = 0, samplesCompleted = 0, samplesDropped = 0;
    };

    explicit GpuTiming(unsigned minimumInterval = 1) : interval(std::max(1u, minimumInterval)) {}
    GpuTiming(const GpuTiming&) = delete;
    GpuTiming& operator=(const GpuTiming&) = delete;

    // The existing diagnostic schedule requests one frame at a time. Later
    // unrequested frames still collect that sample without adding GPU work.
    bool BeginFrame(ID3D11DeviceContext* ctx, bool requestSample)
    {
        EndFrame();
        ++frameId;
        if (!ctx) {
            Reset();
            return false;
        }
        if (context.Get() != ctx) {
            DropPending();
            slots = {};
            context = ctx;
            device.Reset();
            ctx->GetDevice(&device);
            immediate = ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
            nextSample = frameId;
            nextPoll = frameId;
            latest.reset();
        }
        if (!device || !immediate) return false;
        Poll();
        if (!requestSample || frameId < nextSample) return false;
        nextSample = frameId + interval;
        Slot* free = nullptr;
        for (auto& slot : slots) if (slot.state == State::Free) { free = &slot; break; }
        if (!free || !Prepare(*free)) { ++counters.samplesDropped; return false; }
        free->state = State::Recording;
        free->sampleId = frameId;
        free->count = 0;
        free->open = false;
        free->invalid = false;
        free->workOpen = free->workMeasured = false;
        free->workResults = {};
        active = free;
        context->Begin(free->disjoint.Get());
        context->End(free->frameStart.Get());
        context->Begin(free->framePipeline.Get());
        counters.commandsIssued += 3;
        ++counters.samplesStarted;
        return true;
    }

    void Begin(Stage stage)
    {
        if (!active || active->invalid) return;
        if (active->open || active->count == SegmentLimit) { active->invalid = true; return; }
        auto& segment = active->segments[active->count];
        segment.stage = stage;
        context->End(segment.start.Get());
        ++counters.commandsIssued;
        active->open = true;
    }
    void End(Stage stage)
    {
        if (!active || active->invalid) return;
        if (!active->open || active->segments[active->count].stage != stage) { active->invalid = true; return; }
        context->End(active->segments[active->count].finish.Get());
        ++counters.commandsIssued;
        ++active->count;
        active->open = false;
    }
    // CPU wall time and call counts cover every matching work scope in the
    // sampled frame. GPU time and invocation counts cover ONLY the first four
    // scopes per stage, never an estimated total or an on/off savings claim.
    // No clocks, queries or allocation occur on unrequested frames.
    void BeginWork(WorkStage stage)
    {
        if (!active || active->invalid) return;
        const auto index = static_cast<unsigned>(stage);
        if (active->workOpen || index >= WorkStageCount) { active->invalid = true; return; }
        active->workStage = stage;
        auto& result = active->workResults[index];
        ++result.calls;
        active->workMeasured = result.measuredCalls < WorkSegmentsPerStage;
        if (active->workMeasured) {
            auto& segment = active->work[index][result.measuredCalls];
            context->End(segment.start.Get());
            context->Begin(segment.pipeline.Get());
            counters.commandsIssued += 2;
        }
        active->workStart = Clock::now();
        active->workOpen = true;
    }
    void EndWork(WorkStage stage)
    {
        if (!active || active->invalid) return;
        if (!active->workOpen || active->workStage != stage) { active->invalid = true; return; }
        auto& result = active->workResults[static_cast<unsigned>(stage)];
        result.cpuMs += std::chrono::duration<double, std::milli>(Clock::now() - active->workStart).count();
        if (active->workMeasured) {
            auto& segment = active->work[static_cast<unsigned>(stage)][result.measuredCalls];
            context->End(segment.pipeline.Get());
            context->End(segment.finish.Get());
            counters.commandsIssued += 2;
            ++result.measuredCalls;
        }
        active->workOpen = false;
    }
    void EndFrame()
    {
        if (!active) return;
        // Even an abandoned sample must close every query begun on the GPU.
        if (active->workOpen && active->workMeasured) {
            const auto index = static_cast<unsigned>(active->workStage);
            context->End(active->work[index][active->workResults[index].measuredCalls].pipeline.Get());
            ++counters.commandsIssued;
        }
        context->End(active->framePipeline.Get());
        context->End(active->frameFinish.Get());
        context->End(active->disjoint.Get());
        counters.commandsIssued += 3;
        active->invalid = active->invalid || active->open || active->workOpen;
        if (active->invalid) { active->state = State::Free; ++counters.samplesDropped; }
        else active->state = State::Pending;
        active = nullptr;
    }
    const std::optional<Result>& Latest() const { return latest; }
    const Counters& Stats() const { return counters; }
    bool IsSampling() const { return active && !active->invalid; }
    void Reset()
    {
        EndFrame();
        DropPending();
        slots = {};
        context.Reset();
        device.Reset();
        latest.reset();
        immediate = false;
        nextSample = nextPoll = frameId;
    }

private:
    using Clock = std::chrono::steady_clock;
    enum class State { Free, Recording, Pending };
    struct Segment {
        Microsoft::WRL::ComPtr<ID3D11Query> start, finish;
        Stage stage = Stage::Preparation;
    };
    struct WorkSegment {
        Microsoft::WRL::ComPtr<ID3D11Query> start, finish, pipeline;
    };
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint, frameStart, frameFinish, framePipeline;
        std::array<Segment, SegmentLimit> segments;
        std::array<std::array<WorkSegment, WorkSegmentsPerStage>, WorkStageCount> work;
        std::array<WorkResult, WorkStageCount> workResults{};
        Clock::time_point workStart{};
        WorkStage workStage = WorkStage::GuideCapture;
        State state = State::Free;
        std::uint64_t sampleId = 0;
        unsigned count = 0;
        bool open = false, invalid = false, workOpen = false, workMeasured = false;
    };
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    std::array<Slot, SlotCount> slots;
    Slot* active = nullptr;
    std::optional<Result> latest;
    Counters counters;
    std::uint64_t frameId = 0, nextSample = 0, nextPoll = 0;
    unsigned interval;
    bool immediate = false;

    void DropPending()
    {
        for (auto& slot : slots) if (slot.state != State::Free) {
            slot.state = State::Free;
            ++counters.samplesDropped;
        }
    }
    bool Prepare(Slot& slot)
    {
        auto query = [&](Microsoft::WRL::ComPtr<ID3D11Query>& target, D3D11_QUERY type) {
            if (target) return true;
            D3D11_QUERY_DESC desc{type, 0};
            if (FAILED(device->CreateQuery(&desc, &target))) return false;
            ++counters.queriesCreated;
            return true;
        };
        if (!query(slot.disjoint, D3D11_QUERY_TIMESTAMP_DISJOINT) ||
            !query(slot.frameStart, D3D11_QUERY_TIMESTAMP) || !query(slot.frameFinish, D3D11_QUERY_TIMESTAMP) ||
            !query(slot.framePipeline, D3D11_QUERY_PIPELINE_STATISTICS)) return false;
        for (auto& segment : slot.segments)
            if (!query(segment.start, D3D11_QUERY_TIMESTAMP) || !query(segment.finish, D3D11_QUERY_TIMESTAMP)) return false;
        for (auto& stage : slot.work) for (auto& segment : stage)
            if (!query(segment.start, D3D11_QUERY_TIMESTAMP) || !query(segment.finish, D3D11_QUERY_TIMESTAMP) ||
                !query(segment.pipeline, D3D11_QUERY_PIPELINE_STATISTICS)) return false;
        return true;
    }
    template<class T> HRESULT Read(ID3D11Query* query, T& result)
    {
        ++counters.getDataCalls;
        return context->GetData(query, &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    }
    void Poll()
    {
        if (frameId < nextPoll) return;
        Slot* oldest = nullptr;
        for (auto& slot : slots)
            if (slot.state == State::Pending && frameId - slot.sampleId >= PollDelay &&
                (!oldest || slot.sampleId < oldest->sampleId)) oldest = &slot;
        if (!oldest) return;
        nextPoll = frameId + PollDelay;
        auto abandon = [&] { oldest->state = State::Free; ++counters.samplesDropped; };
        if (frameId - oldest->sampleId > 240) { abandon(); return; }
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        HRESULT hr = Read(oldest->disjoint.Get(), disjoint);
        if (hr == S_FALSE) return;
        if (FAILED(hr) || disjoint.Disjoint || !disjoint.Frequency) { abandon(); return; }
        UINT64 start = 0, finish = 0;
        hr = Read(oldest->frameStart.Get(), start);
        if (hr == S_FALSE) return;
        if (FAILED(hr)) { abandon(); return; }
        hr = Read(oldest->frameFinish.Get(), finish);
        if (hr == S_FALSE) return;
        if (FAILED(hr) || finish < start) { abandon(); return; }
        Result result;
        result.sampleId = oldest->sampleId;
        result.frameSpanMs = double(finish - start) * 1000. / double(disjoint.Frequency);
        result.work = oldest->workResults;
        hr = Read(oldest->framePipeline.Get(), result.framePipeline);
        if (hr == S_FALSE) return;
        if (FAILED(hr)) { abandon(); return; }
        for (unsigned i = 0; i < oldest->count; ++i) {
            const auto& segment = oldest->segments[i];
            UINT64 begin = 0, end = 0;
            hr = Read(segment.start.Get(), begin);
            if (hr == S_FALSE) return;
            if (FAILED(hr)) { abandon(); return; }
            hr = Read(segment.finish.Get(), end);
            if (hr == S_FALSE) return;
            if (FAILED(hr) || end < begin || begin < start || end > finish) { abandon(); return; }
            const double ms = double(end - begin) * 1000. / double(disjoint.Frequency);
            if (segment.stage == Stage::Preparation) { result.preparationMs += ms; ++result.preparationSegments; }
            else { result.resolveMs += ms; ++result.resolveSegments; }
        }
        for (unsigned stage = 0; stage < WorkStageCount; ++stage) {
            auto& work = result.work[stage];
            for (unsigned i = 0; i < work.measuredCalls; ++i) {
                const auto& segment = oldest->work[stage][i];
                UINT64 begin = 0, end = 0;
                hr = Read(segment.start.Get(), begin);
                if (hr == S_FALSE) return;
                if (FAILED(hr)) { abandon(); return; }
                hr = Read(segment.finish.Get(), end);
                if (hr == S_FALSE) return;
                if (FAILED(hr) || end < begin || begin < start || end > finish) { abandon(); return; }
                D3D11_QUERY_DATA_PIPELINE_STATISTICS pipeline{};
                hr = Read(segment.pipeline.Get(), pipeline);
                if (hr == S_FALSE) return;
                if (FAILED(hr)) { abandon(); return; }
                work.gpuSubsetMs += double(end - begin) * 1000. / double(disjoint.Frequency);
                work.vsInvocations += pipeline.VSInvocations;
                work.psInvocations += pipeline.PSInvocations;
            }
        }
        if (!latest || result.sampleId > latest->sampleId) latest = result;
        ++counters.samplesCompleted;
        oldest->state = State::Free;
    }
};
}
