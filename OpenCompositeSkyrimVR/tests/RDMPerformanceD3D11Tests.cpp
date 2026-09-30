#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "OpenOVR/Compositor/RDMRenderScope.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
void oovr_log_raw(const char*, long, const char*, const char* message) { std::puts(message); }
void oovr_log_raw_format(const char*, long, const char*, const char* format, ...) {
    va_list args; va_start(args, format); std::vprintf(format, args); va_end(args); std::puts("");
}
static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static void HR(HRESULT result) {
    if (FAILED(result)) {
        std::printf("HRESULT=0x%08X\n", unsigned(result));
        throw std::runtime_error("D3D11 operation failed");
    }
}
static ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> shader, errors;
    const auto result = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr,
        entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(result) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    HR(result); return shader;
}
template<class T> static void ReadQuery(ID3D11DeviceContext* context, ID3D11Query* query, T& result) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    for (;;) {
        const auto status = context->GetData(query, &result, sizeof(result), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (status == S_OK) return;
        HR(status);
        Require(Clock::now() < deadline, "timed out retrieving completed query");
        Sleep(1);
    }
}

// Optional counters allow the identical fixture to build against the archived
// baseline as well as the candidate without changing either implementation.
template<class T> static auto PrintGuideCounters(const T& stats, int)
    -> decltype(stats.guideStateQueries, stats.emptyGuideSkips, void()) {
    std::printf(" guideQueries=%u emptyGuideSkips=%u", stats.guideStateQueries, stats.emptyGuideSkips);
}
template<class T> static void PrintGuideCounters(const T&, long) {}
template<class T> static auto PrintMaskCounters(const T& stats, int)
    -> decltype(stats.maskPreparations, stats.maskReuses, void()) {
    std::printf(" maskPreparations=%u maskReuses=%u", stats.maskPreparations, stats.maskReuses);
}
template<class T> static void PrintMaskCounters(const T&, long) {}

template<class T> static auto PrintGuideReadCounters(const T& stats, int)
    -> decltype(stats.guideDepthReads, stats.guideTargetReads, stats.guideHazardReads, void()) {
    std::printf(" guideDepthReads=%u guideTargetReads=%u guideHazardReads=%u",
        stats.guideDepthReads, stats.guideTargetReads, stats.guideHazardReads);
}
template<class T> static void PrintGuideReadCounters(const T&, long) {
    std::printf(" guideDepthReads=unavailable guideTargetReads=unavailable guideHazardReads=unavailable");
}

struct Fixture {
    UINT width = 128, height = 64;
    static constexpr UINT draws = 1400, batchDraws = 32, expensiveDraws = 24;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> color, depth;
    ComPtr<ID3D11RenderTargetView> colorView;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader, alternatePixelShader, expensivePixelShader;
    ComPtr<ID3D11DepthStencilState> depthWrite, depthEqual;
    ComPtr<ID3D11RasterizerState> rasterizer, alternateRasterizer;
    ComPtr<ID3D11BlendState> explicitBlend;
    ComPtr<ID3D11Query> disjoint, begin, end, pipeline;
    RDMRenderScope rdm;
    struct Measurement { double cpuMs, gpuMs; RDMRenderScope::Statistics stats; D3D11_QUERY_DATA_PIPELINE_STATISTICS pipeline; };

    Fixture(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver) {
        const D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_11_0;
        D3D_FEATURE_LEVEL actual;
        HR(D3D11CreateDevice(adapter, driver, nullptr, 0, &requested, 1, D3D11_SDK_VERSION,
            &device, &actual, &context));
        ComPtr<ID3D11Multithread> threading; HR(context.As(&threading));
        threading->SetMultithreadProtected(TRUE);
        Require(threading->GetMultithreadProtected(), "context protection was not enabled");
        Require(ocu_vrs_guard::InstallShaderCapture(device.Get()), "shader capture installation failed");
        constexpr char source[] = R"HLSL(
            float4 VS(uint id:SV_VertexID):SV_POSITION {
                return float4(id==2?3:-1,id==1?-3:1,0.4,1);
            }
            float4 PS():SV_Target0 { return float4(.25,.5,.75,1); }
            float4 AlternatePS():SV_Target0 { return float4(.75,.5,.25,1); }
            float4 ExpensivePS(float4 position:SV_POSITION):SV_Target0 {
                float3 v = float3(position.xy * .0001, .37);
                [loop] for (uint i=0;i<128;++i) {
                    v = frac(sin(v.yzx * float3(1.173, 1.331, 1.517) + v.zxy + float(i)*.001) * .731 + .27);
                }
                return float4(v,1);
            }
        )HLSL";
        const auto vs = Compile(source, "VS", "vs_5_0"), ps = Compile(source, "PS", "ps_5_0");
        HR(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader));
        HR(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &pixelShader));
        Require(ocu_vrs_guard::ShaderColorOutputs(pixelShader.Get()) == 1, "color shader was not classified");
        const auto alternate = Compile(source, "AlternatePS", "ps_5_0");
        const auto expensive = Compile(source, "ExpensivePS", "ps_5_0");
        HR(device->CreatePixelShader(alternate->GetBufferPointer(), alternate->GetBufferSize(), nullptr, &alternatePixelShader));
        HR(device->CreatePixelShader(expensive->GetBufferPointer(), expensive->GetBufferSize(), nullptr, &expensivePixelShader));
        Require(ocu_vrs_guard::ShaderColorOutputs(expensivePixelShader.Get()) == 1, "expensive shader was not classified");
        CreateTargets();
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        HR(device->CreateDepthStencilState(&ds, &depthWrite));
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_EQUAL;
        HR(device->CreateDepthStencilState(&ds, &depthEqual));
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        HR(device->CreateRasterizerState(&rs, &rasterizer));
        rs.FrontCounterClockwise = TRUE;
        HR(device->CreateRasterizerState(&rs, &alternateRasterizer));
        D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HR(device->CreateBlendState(&bd, &explicitBlend));
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        HR(device->CreateQuery(&query, &disjoint));
        query.Query = D3D11_QUERY_TIMESTAMP;
        HR(device->CreateQuery(&query, &begin)); HR(device->CreateQuery(&query, &end));
        query.Query = D3D11_QUERY_PIPELINE_STATISTICS; HR(device->CreateQuery(&query, &pipeline));
        Require(RDMRenderScope::PrepareDrawHooks(device.Get()), "draw hook preparation failed");
    }
    void CreateTargets() {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        colorView.Reset(); depthView.Reset(); color.Reset(); depth.Reset();
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = width; texture.Height = height;
        texture.MipLevels = texture.ArraySize = texture.SampleDesc.Count = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        HR(device->CreateTexture2D(&texture, nullptr, &color));
        HR(device->CreateRenderTargetView(color.Get(), nullptr, &colorView));
        texture.Format = DXGI_FORMAT_R24G8_TYPELESS;
        texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        HR(device->CreateTexture2D(&texture, nullptr, &depth));
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        HR(device->CreateDepthStencilView(depth.Get(), &view, &depthView));
    }
    void Bind(bool equal, bool colorEnabled) {
        auto* target = colorView.Get();
        context->OMSetRenderTargets(1, &target, depthView.Get());
        RDMRenderScope::NotifyTargets(context.Get());
        context->OMSetDepthStencilState(equal ? depthEqual.Get() : depthWrite.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->RSSetState(rasterizer.Get());
        D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 0.999998f};
        context->RSSetViewports(1, &viewport);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(colorEnabled ? pixelShader.Get() : nullptr, nullptr, 0);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    Measurement Frame(unsigned workload, bool enabled) {
        rdm.EndFrame();
        const float black[4]{};
        context->ClearRenderTargetView(colorView.Get(), black);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1, 0);
        context->Begin(disjoint.Get()); context->End(begin.Get());
        context->Begin(pipeline.Get());
        const auto cpuBegin = Clock::now();
        if (enabled) {
            const float centers[4]{.5f, .5f, .5f, .5f};
            Require(rdm.Arm(context.Get(), depth.Get(), color.Get(), width, height,
                {0, 0, int(width / 2), int(height)}, {int(width / 2), 0, int(width / 2), int(height)},
                {.1f, .3f, false, true,
                    {ocu_foveation::Rate::X1x1, ocu_foveation::Rate::X2x2, ocu_foveation::Rate::X4x2}},
                centers, false), "RDM arm failed");
        }
        if (workload != 0) { Bind(false, false); context->Draw(3, 0); }
        Bind(workload == 2 || workload == 5, true);
        if (workload == 5) context->PSSetShader(expensivePixelShader.Get(), nullptr, 0);
        const UINT currentDraws = workload == 5 ? expensiveDraws : draws;
        for (UINT index = 0; index < currentDraws; ++index) {
            if (workload == 3 || workload == 4) {
                // Seeded invalidation workload. Real engines change draw state;
                // the old fixture hid query cost by holding every state constant.
                context->PSSetShader(index % 2 ? pixelShader.Get() : alternatePixelShader.Get(), nullptr, 0);
                context->OMSetBlendState(index % 2 ? nullptr : explicitBlend.Get(), nullptr, 0xffffffffu);
                context->RSSetState(index % 2 ? rasterizer.Get() : alternateRasterizer.Get());
                const D3D11_RECT scissor{LONG(index % 2), 0, LONG(width), LONG(height)};
                context->RSSetScissorRects(1, &scissor);
                if (workload == 4) {
                    auto* target = colorView.Get();
                    context->OMSetRenderTargets(1, &target, depthView.Get());
                    RDMRenderScope::NotifyTargets(context.Get());
                    context->OMSetDepthStencilState(depthWrite.Get(), 0);
                    context->SetPredication(nullptr, FALSE);
                    context->SOSetTargets(0, nullptr, nullptr);
                }
            }
            context->Draw(3, 0);
            if (enabled && workload == 2 && (index + 1) % batchDraws == 0)
                rdm.BeforeRead(color.Get());
        }
        rdm.EndFrame();
        const auto cpuEnd = Clock::now();
        context->End(pipeline.Get());
        context->End(end.Get()); context->End(disjoint.Get());
        // Submit and retrieve only after timing ends. The previous frame is
        // fully retired before the next begins; no readbacks occur in a draw loop.
        context->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        UINT64 start = 0, finish = 0;
        ReadQuery(context.Get(), disjoint.Get(), frequency);
        ReadQuery(context.Get(), begin.Get(), start); ReadQuery(context.Get(), end.Get(), finish);
        Require(!frequency.Disjoint && frequency.Frequency != 0 && finish >= start,
            "GPU timestamp interval was disjoint");
        Measurement measurement{std::chrono::duration<double, std::milli>(cpuEnd - cpuBegin).count(),
            double(finish - start) * 1000.0 / double(frequency.Frequency), enabled ? rdm.Stats() : RDMRenderScope::Statistics{}, {}};
        ReadQuery(context.Get(), pipeline.Get(), measurement.pipeline);
        if (enabled) {
            Require(measurement.stats.draws == currentDraws + (workload != 0), "draw interception count mismatch");
            Require(measurement.stats.maskedDraws == ((workload == 2 || workload == 5) ? currentDraws : 0), "unexpected masked draw admission");
            Require(measurement.stats.guideDraws == (workload != 0 ? 1u : 0u), "depth-guide production count mismatch");
            if (workload == 3 || workload == 4) Require(measurement.stats.guideInvalidationDraws == draws, "state-change workload did not replay each depth-writing draw");
        }
        return measurement;
    }
    void Run() {
        constexpr const char* names[]{"empty-guide-depth-writes", "owned-guide-depth-writes", "masked-consumer-batches", "state-changing-owned-guide", "full-state-changing-owned-guide", "expensive-color-scene"};
        std::printf("FIXTURE width=%u height=%u draws=%u batchDraws=%u debugLayer=0 detailedDiagnostics=0 protected=1\n",
            width, height, draws, batchDraws);
        constexpr unsigned sampleCount = 9;
        for (unsigned workload = 0; workload != 6; ++workload) {
            if (workload == 5) { rdm.EndFrame(); width = 1024; height = 512; CreateTargets(); }
            std::printf("WORKLOAD name=%s width=%u height=%u draws=%u shaderIterations=%u\n",
                names[workload], width, height, workload == 5 ? expensiveDraws : draws, workload == 5 ? 128u : 0u);
            // Alternate on/off ordering to avoid giving the enabled path every
            // warmed GPU clock/cache state after a long uninterrupted off run.
            for (unsigned warmup = 0; warmup != 4; ++warmup) {
                Frame(workload, warmup % 2 != 0); Frame(workload, warmup % 2 == 0);
            }
            std::vector<double> cpu[2], gpu[2];
            for (unsigned sample = 0; sample != sampleCount; ++sample) {
                for (unsigned order = 0; order != 2; ++order) {
                    const bool enabled = (sample + order) % 2 != 0;
                    const auto result = Frame(workload, enabled);
                    cpu[enabled].push_back(result.cpuMs); gpu[enabled].push_back(result.gpuMs);
                    const auto& stats = result.stats;
                    std::printf("SAMPLE workload=%s rdm=%u sample=%u cpuMs=%.6f gpuMs=%.6f draws=%u masked=%u queries=%u replays=%u batches=%u resolves=%u",
                        names[workload], enabled, sample, result.cpuMs, result.gpuMs, stats.draws,
                        stats.maskedDraws, stats.stateQueries, stats.guideInvalidationDraws, stats.batches, stats.resolves);
                    PrintGuideCounters(stats, 0); PrintMaskCounters(stats, 0); PrintGuideReadCounters(stats, 0);
                    std::printf(" psInvocations=%llu vsInvocations=%llu iaPrimitives=%llu\n",
                        result.pipeline.PSInvocations, result.pipeline.VSInvocations, result.pipeline.IAPrimitives);
                }
            }
            for (unsigned enabled = 0; enabled != 2; ++enabled) {
                std::sort(cpu[enabled].begin(), cpu[enabled].end()); std::sort(gpu[enabled].begin(), gpu[enabled].end());
                std::printf("MEDIAN workload=%s rdm=%u cpuMs=%.6f gpuMs=%.6f samples=%u\n",
                    names[workload], enabled, cpu[enabled][sampleCount / 2], gpu[enabled][sampleCount / 2], sampleCount);
            }
        }
    }
};
int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        const bool hardware = argc > 1 && std::strcmp(argv[1], "--hardware") == 0;
        ComPtr<IDXGIAdapter1> adapter;
        if (hardware) {
            ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC1 description{}; HR(candidate->GetDesc1(&description));
                if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                std::printf("ADAPTER name=%ls vendor=0x%04X device=0x%04X\n",
                    description.Description, description.VendorId, description.DeviceId);
                adapter = candidate; break;
            }
            Require(adapter != nullptr, "no hardware adapter found");
        } else std::puts("ADAPTER name=WARP software=1");
        std::puts("Synthetic D3D11 workload; timing includes production RDM work, not Skyrim or headset performance.");
        Fixture fixture(adapter.Get(), hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP);
        fixture.Run();
        std::puts("RDM PERFORMANCE FIXTURE PASS"); return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what()); return 1;
    }
}
