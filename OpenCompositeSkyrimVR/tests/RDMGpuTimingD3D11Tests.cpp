#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
#include "OpenOVR/Compositor/RDMGpuTiming.h"
using Microsoft::WRL::ComPtr;

static void Require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void HR(HRESULT result) { if (FAILED(result)) { std::printf("HRESULT=%08X\n", unsigned(result)); throw std::runtime_error("D3D11 call failed"); } }
static constexpr unsigned QueriesPerSlot = 4 + ocu_rdm::GpuTiming::SegmentLimit * 2 +
    ocu_rdm::GpuTiming::WorkStageCount * ocu_rdm::GpuTiming::WorkSegmentsPerStage * 3;
static ocu_rdm::GpuTiming::Result Collect(ocu_rdm::GpuTiming& timing, ID3D11DeviceContext* context) {
    context->Flush(); // Fixture only; production polling must never flush.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!timing.Latest()) {
        Require(std::chrono::steady_clock::now() < deadline, "asynchronous work result unavailable");
        Require(!timing.BeginFrame(context, false), "unrequested work sample started"); timing.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return *timing.Latest();
}
static void Run(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type) {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(adapter, type, nullptr, D3D11_CREATE_DEVICE_DEBUG, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<ID3D11InfoQueue> debug; HR(device.As(&debug));
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = 256;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> target;
    HR(device->CreateTexture2D(&desc, nullptr, &texture)); HR(device->CreateRenderTargetView(texture.Get(), nullptr, &target));
    const float first[]{.1f, .2f, .3f, 1.f}, second[]{.7f, .5f, .3f, 1.f};
    using Timing = ocu_rdm::GpuTiming; using Stage = Timing::Stage;
    using WorkStage = Timing::WorkStage;
    {
        Timing disabled;
        for (unsigned frame = 0; frame < 8; ++frame) {
            Require(!disabled.BeginFrame(context.Get(), false), "disabled frame sampled");
            disabled.Begin(Stage::Preparation); disabled.End(Stage::Preparation); disabled.EndFrame();
            disabled.BeginWork(WorkStage::GuideCapture); disabled.EndWork(WorkStage::GuideCapture);
        }
        Require(!disabled.Stats().queriesCreated && !disabled.Stats().commandsIssued && !disabled.Stats().getDataCalls,
            "disabled sampler touched GPU queries");
    }
    {
        Timing timing(120);
        Require(timing.BeginFrame(context.Get(), true), "initial sample did not start");
        timing.Begin(Stage::Preparation); context->ClearRenderTargetView(target.Get(), first); timing.End(Stage::Preparation);
        timing.Begin(Stage::Resolve); context->ClearRenderTargetView(target.Get(), second); timing.End(Stage::Resolve);
        timing.EndFrame(); Require(!timing.Latest(), "same-frame query result exposed");
        const auto created = timing.Stats().queriesCreated, issued = timing.Stats().commandsIssued;
        Require(created == QueriesPerSlot, "sample query allocation not bounded to one slot");
        for (unsigned frame = 0; frame < Timing::PollDelay - 1; ++frame) {
            Require(!timing.BeginFrame(context.Get(), false), "unscheduled frame sampled"); timing.EndFrame();
            Require(!timing.Stats().getDataCalls, "polled before three subsequent frames");
        }
        // Only the fixture submits/drains work. The production helper never
        // Flushes, waits, or asks GetData to flush pending GPU commands.
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!timing.Latest()) {
            Require(std::chrono::steady_clock::now() < deadline, "asynchronous timing result unavailable");
            Require(!timing.BeginFrame(context.Get(), false), "extra sample started while waiting"); timing.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto& result = *timing.Latest();
        Require(result.sampleId == 1 && result.preparationSegments == 1 && result.resolveSegments == 1, "sample identity/stage counts wrong");
        Require(std::isfinite(result.preparationMs) && std::isfinite(result.resolveMs) && std::isfinite(result.frameSpanMs), "invalid timing values");
        Require(result.preparationMs >= 0 && result.resolveMs >= 0 &&
            result.frameSpanMs + 1.e-9 >= result.preparationMs + result.resolveMs, "stage sums exceed frame span");
        Require(timing.Stats().queriesCreated == created && timing.Stats().commandsIssued == issued, "unsampled frames submitted query commands");
        const auto polls = timing.Stats().getDataCalls;
        for (unsigned i = 0; i < 10; ++i) { timing.BeginFrame(context.Get(), false); timing.EndFrame(); }
        Require(timing.Stats().getDataCalls == polls, "no-pending frames polled queries");
        std::printf("async sample=%llu prep=%.6fms resolve=%.6fms frameSpan=%.6fms queries=%llu reads=%llu\n",
            result.sampleId, result.preparationMs, result.resolveMs, result.frameSpanMs, created, polls);
    }
    {
        Timing overflow(1);
        Require(overflow.BeginFrame(context.Get(), true), "overflow sample did not start");
        for (unsigned i = 0; i <= Timing::SegmentLimit; ++i) { overflow.Begin(Stage::Resolve); overflow.End(Stage::Resolve); }
        Require(!overflow.IsSampling(), "segment overflow kept sampling"); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 1 && !overflow.Latest(), "overflow published a partial result");
        Require(overflow.Stats().queriesCreated == QueriesPerSlot, "overflow allocated more query slots");
        Require(overflow.BeginFrame(context.Get(), true), "sampler failed to recover after overflow");
        overflow.Begin(Stage::Resolve); overflow.End(Stage::Preparation); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 2, "mismatched stage was not abandoned");
        Require(overflow.BeginFrame(context.Get(), true), "new sample after mismatched stage failed");
        overflow.Begin(Stage::Resolve); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 3, "open segment was not abandoned");
        Require(overflow.BeginFrame(context.Get(), true), "work mismatch sample did not start");
        overflow.BeginWork(WorkStage::GuideCapture); overflow.EndWork(WorkStage::SceneMasked); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 4, "mismatched work stage was not abandoned");
        Require(overflow.BeginFrame(context.Get(), true), "work nesting sample did not start");
        overflow.BeginWork(WorkStage::GuideCapture); overflow.BeginWork(WorkStage::SceneMasked); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 5, "nested work stage was not abandoned");
        Require(overflow.BeginFrame(context.Get(), true), "unclosed work sample did not start");
        overflow.BeginWork(WorkStage::SceneFullRate); overflow.EndFrame();
        Require(overflow.Stats().samplesDropped == 6, "open work stage was not abandoned");
    }
    {
        Timing timing(1);
        for (unsigned i = 0; i < Timing::SlotCount; ++i) { Require(timing.BeginFrame(context.Get(), true), "pool slot missing"); timing.EndFrame(); }
        const auto commands = timing.Stats().commandsIssued;
        Require(timing.Stats().queriesCreated == Timing::SlotCount * QueriesPerSlot, "pool allocation escaped fixed bound");
        context->Flush();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (timing.Stats().samplesCompleted < Timing::SlotCount) {
            Require(std::chrono::steady_clock::now() < deadline, "pending pool did not drain without new requests");
            Require(!timing.BeginFrame(context.Get(), false), "unrequested frame allocated a sample"); timing.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(timing.Stats().commandsIssued == commands && !timing.Stats().samplesDropped, "pending-only polls issued GPU commands or dropped samples");
        const auto polls = timing.Stats().getDataCalls;
        for (unsigned i = 0; i < 8; ++i) { timing.BeginFrame(context.Get(), false); timing.EndFrame(); }
        Require(timing.Stats().commandsIssued == commands && timing.Stats().getDataCalls == polls, "drained disabled sampler performed GPU work");
        Require(timing.BeginFrame(context.Get(), true), "reset fixture sample missing"); timing.EndFrame();
        timing.Reset();
        Require(timing.Stats().samplesDropped == 1 && !timing.Latest(), "reset did not discard pending results");
    }
    {
        // Controlled raster coverage verifies the counters themselves. These
        // are separate fixture draws, not replays of a live game frame and not
        // a performance claim about RDM's real shaders or coverage rules.
        static constexpr char shader[] =
            "float4 VS(uint id:SV_VertexID):SV_Position {"
            "return float4(id==2 ? 3:-1, id==1 ? 3:-1,0,1); }"
            "float4 PS():SV_Target { return float4(0.2,0.4,0.6,1); }";
        ComPtr<ID3DBlob> vsCode, psCode, errors;
        HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "VS", "vs_5_0", 0, 0, &vsCode, &errors));
        HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr, "PS", "ps_5_0", 0, 0, &psCode, &errors));
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        HR(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
        HR(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
        D3D11_RASTERIZER_DESC rasterDesc{}; rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.ScissorEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster; HR(device->CreateRasterizerState(&rasterDesc, &raster));
        D3D11_DEPTH_STENCIL_DESC depthDesc{}; depthDesc.DepthEnable = FALSE;
        ComPtr<ID3D11DepthStencilState> depth; HR(device->CreateDepthStencilState(&depthDesc, &depth));
        auto* rt = target.Get(); context->OMSetRenderTargets(1, &rt, nullptr);
        context->OMSetDepthStencilState(depth.Get(), 0); context->RSSetState(raster.Get());
        D3D11_VIEWPORT viewport{0,0,32,32,0,1}; context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
        Timing timing;
        Require(timing.BeginFrame(context.Get(), true), "work sample did not start");
        constexpr unsigned CallsPerStage = 1400;
        for (unsigned i = 0; i < Timing::WorkStageCount; ++i) {
            const auto stage = static_cast<WorkStage>(i);
            const D3D11_RECT rectangle{0, 0, stage == WorkStage::SceneMasked ? 16 : 32, 32};
            context->RSSetScissorRects(1, &rectangle);
            for (unsigned call = 0; call < CallsPerStage; ++call) {
                timing.BeginWork(stage); context->Draw(3, 0); timing.EndWork(stage);
            }
        }
        timing.EndFrame();
        const auto result = Collect(timing, context.Get());
        Require(timing.Stats().queriesCreated == QueriesPerSlot, "many draws allocated unbounded queries");
        Require(timing.Stats().commandsIssued == 6 + Timing::WorkStageCount * Timing::WorkSegmentsPerStage * 4,
            "many draws issued unbounded query commands");
        std::uint64_t subsetVS = 0, subsetPS = 0;
        for (unsigned i = 0; i < Timing::WorkStageCount; ++i) {
            const auto& work = result.work[i];
            Require(work.calls == CallsPerStage && work.measuredCalls == Timing::WorkSegmentsPerStage, "call count/subset boundary wrong");
            Require(work.cpuMs > 0 && std::isfinite(work.cpuMs) && work.gpuSubsetMs >= 0 && std::isfinite(work.gpuSubsetMs), "invalid work timing");
            Require(work.vsInvocations == 3 * Timing::WorkSegmentsPerStage && work.psInvocations > 0, "pipeline invocation query wrong");
            subsetVS += work.vsInvocations; subsetPS += work.psInvocations;
            std::printf("work stage=%u calls=%u measured=%u CPU=%.6fms subsetGPU=%.6fms VS=%llu PS=%llu\n",
                i, work.calls, work.measuredCalls, work.cpuMs, work.gpuSubsetMs, work.vsInvocations, work.psInvocations);
        }
        const auto& masked = result.work[static_cast<unsigned>(WorkStage::SceneMasked)];
        const auto& full = result.work[static_cast<unsigned>(WorkStage::SceneFullRate)];
        std::printf("work frame VS=%llu PS=%llu subsetVS=%llu subsetPS=%llu calls=%u queryCommands=%llu\n", result.framePipeline.VSInvocations,
            result.framePipeline.PSInvocations, subsetVS, subsetPS, CallsPerStage * Timing::WorkStageCount, timing.Stats().commandsIssued);
        Require(masked.psInvocations * 2 == full.psInvocations, "controlled half-coverage draw did not halve PS invocations");
        // Vertex caches can reuse results across draws outside query
        // boundaries. VSInvocations is executed work, not submitted vertices.
        Require(result.framePipeline.IAVertices == 3 * CallsPerStage * Timing::WorkStageCount &&
            result.framePipeline.VSInvocations >= subsetVS &&
            result.framePipeline.VSInvocations <= result.framePipeline.IAVertices,
            "frame-wide input/executed vertex counters invalid");
        Require(result.framePipeline.PSInvocations == subsetPS * CallsPerStage / Timing::WorkSegmentsPerStage,
            "overlapping frame-wide and subset PS queries disagree");
    }
    context->ClearState();
    for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; HR(debug->GetMessage(i, nullptr, &size)); std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data()); HR(debug->GetMessage(i, message, &size));
        // Overflow deliberately abandons results. D3D11 labels valid reuse of
        // those queries unusual; no wait/readback is introduced to silence it.
        if (message->ID == D3D11_MESSAGE_ID_QUERY_BEGIN_ABANDONING_PREVIOUS_RESULTS ||
            message->ID == D3D11_MESSAGE_ID_QUERY_END_ABANDONING_PREVIOUS_RESULTS) continue;
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) { std::puts(message->pDescription); Require(false, "D3D11 warnings/errors"); }
    }
    std::puts("RDM GPU TIMING PASS: delayed asynchronous results, disabled no-ops, bounded work subsets, real PS/VS counters, overlapping queries, abandonment and recovery");
}
int main() { try {
    Run(nullptr, D3D_DRIVER_TYPE_WARP);
    ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter; HR(factory->EnumAdapters1(0, &adapter));
    DXGI_ADAPTER_DESC1 desc{}; HR(adapter->GetDesc1(&desc)); std::printf("Hardware: %ls\n", desc.Description);
    Run(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN); return 0;
} catch (const std::exception& error) { std::printf("FAIL: %s\n", error.what()); return 1; } }
