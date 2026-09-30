#include "DrvOpenXR/DapaComputeState.h"
#include "DrvOpenXR/DapaWarpShader.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT result) { Check(SUCCEEDED(result), "D3D11 operation failed"); }
static bool verbose = false;
static void Trace(const char* message) {
    if (verbose) { std::fprintf(stderr, "STATE TEST: %s\n", message); std::fflush(stderr); }
}
static void Equal(const Pixel& a, const Pixel& b, const char* message) {
    for (unsigned i = 0; i < 4; ++i) Check(std::abs(a[i] - b[i]) < 0.0001f, message);
}
struct WarpConstants {
    float transform[16]{};
    float resolution[2]{8, 8}, nearZ = 1, farZ = 100;
    float fov[4]{-1, 1, 1, -1};
    float depthScale = 1, edgeFade = 0, nearFade = 0, tint = 0;
    float depthSize[2]{8, 8}, flip[2]{};
    float projection[16]{}, inverse[16]{}, predictionValid = 0, padding[3]{};
};
static_assert(sizeof(WarpConstants) == 272);

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView> rtv;
};
struct Buffer {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

class Fixture {
public:
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11ComputeShader> producer, warp[2];
    ComPtr<ID3D11Buffer> producerConstants, producerExtraConstants, warpConstants, warpBlackoutConstants;
    ComPtr<ID3D11SamplerState> producerSampler, warpSampler;
    Texture inputs[3], warpColor, warpDepth, warpOutput[3], graphicsOutput;
    Buffer results, auxiliary[2], sentinel;
    static constexpr Pixel expected{27, 42, 59, 74};

    explicit Fixture(D3D_DRIVER_TYPE driver) {
        Trace("create device");
        D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context));
        Check(level >= D3D_FEATURE_LEVEL_11_0, "compute test requires feature level 11");
        HR(device.As(&debug));
        Trace("compile producer");
        const char* producerSource = R"HLSL(
Texture2D<float4> first : register(t0);
Texture2D<float4> second : register(t1);
Texture2D<float4> third : register(t2);
SamplerState pointSampler : register(s0);
cbuffer Params : register(b0) { float gain; float bias; float2 unused; };
cbuffer ExtraParams : register(b1) { float4 extraValue; };
AppendStructuredBuffer<float4> results : register(u0);
RWStructuredBuffer<float4> auxiliary : register(u1);
RWStructuredBuffer<float4> extra : register(u2);
[numthreads(1,1,1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    float4 value = first.SampleLevel(pointSampler, float2(0.5,0.5), 0) * gain
        + second.Load(int3(0,0,0)) + third.Load(int3(0,0,0)) + bias + extraValue;
    results.Append(value); auxiliary[0] = value; extra[0] = value;
}
)HLSL";
        producer = Shader(producerSource);
        Trace("compile shipping warp");
        warp[0] = Shader(s_warpShaderHLSL);
        const D3D_SHADER_MACRO capture[] = {{"DAPA_CAPTURE", "1"}, {nullptr, nullptr}};
        warp[1] = Shader(s_warpShaderHLSL, capture);
        Trace("create fixture resources");

        const Pixel parameters{2, 3, 0, 0};
        producerConstants = ConstantBuffer(&parameters, sizeof(parameters));
        const Pixel extraParameters{5, 7, 11, 13};
        producerExtraConstants = ConstantBuffer(&extraParameters, sizeof(extraParameters));
        const WarpConstants parametersWarp;
        warpConstants = ConstantBuffer(&parametersWarp, sizeof(parametersWarp));
        const std::array<float, 8> disabledBlackout{};
        warpBlackoutConstants = ConstantBuffer(disabledBlackout.data(), sizeof(disabledBlackout));
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        HR(device->CreateSamplerState(&sampler, &producerSampler));
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        HR(device->CreateSamplerState(&sampler, &warpSampler));

        const Pixel a[] = {{0, 0, 0, 0}, {4, 5, 6, 7}};
        const Pixel b[] = {{10, 20, 30, 40}, {10, 20, 30, 40}};
        const Pixel c[] = {{1, 2, 3, 4}, {1, 2, 3, 4}};
        inputs[0] = MakeTexture(2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, a, sizeof(a));
        inputs[1] = MakeTexture(2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, b, sizeof(b));
        inputs[2] = MakeTexture(2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, c, sizeof(c));
        std::array<Pixel, 64> colors;
        colors.fill(Pixel{0.25f, 0.5f, 0.75f, 1});
        std::array<float, 64> depths;
        depths.fill(0.5f);
        warpColor = MakeTexture(8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, colors.data(), 8 * sizeof(Pixel));
        warpDepth = MakeTexture(8, 8, DXGI_FORMAT_R32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE, depths.data(), 8 * sizeof(float));
        for (auto& output : warpOutput)
            output = MakeTexture(8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        graphicsOutput = MakeTexture(8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_RENDER_TARGET);
        results = StructuredBuffer(true);
        for (auto& output : auxiliary) output = StructuredBuffer(false);
        sentinel = StructuredBuffer(false);
        Trace("fixture ready");
    }

    ComPtr<ID3D11ComputeShader> Shader(const char* source, const D3D_SHADER_MACRO* macros = nullptr,
        ID3D11ClassLinkage* linkage = nullptr) {
        ComPtr<ID3DBlob> code, errors;
        const HRESULT result = D3DCompile(source, std::strlen(source), "DapaComputeStateRegression", macros,
            nullptr, "CSMain", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &code, &errors);
        if (FAILED(result) && errors) std::fprintf(stderr, "%s\n", static_cast<char*>(errors->GetBufferPointer()));
        HR(result);
        ComPtr<ID3D11ComputeShader> shader;
        HR(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), linkage, &shader));
        return shader;
    }
    ComPtr<ID3D11Buffer> ConstantBuffer(const void* contents, UINT size) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = size;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        const D3D11_SUBRESOURCE_DATA initial{contents, 0, 0};
        ComPtr<ID3D11Buffer> buffer;
        HR(device->CreateBuffer(&desc, &initial, &buffer));
        return buffer;
    }
    Texture MakeTexture(UINT width, UINT height, DXGI_FORMAT format, UINT flags,
        const void* contents = nullptr, UINT rowPitch = 0) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.BindFlags = flags;
        const D3D11_SUBRESOURCE_DATA initial{contents, rowPitch, 0};
        Texture result;
        HR(device->CreateTexture2D(&desc, contents ? &initial : nullptr, &result.texture));
        if (flags & D3D11_BIND_SHADER_RESOURCE) HR(device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.srv));
        if (flags & D3D11_BIND_UNORDERED_ACCESS) HR(device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.uav));
        if (flags & D3D11_BIND_RENDER_TARGET) HR(device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.rtv));
        return result;
    }
    Buffer StructuredBuffer(bool append) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = 8 * sizeof(Pixel); desc.StructureByteStride = sizeof(Pixel);
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        const std::array<Pixel, 8> zeros{};
        const D3D11_SUBRESOURCE_DATA initial{zeros.data(), 0, 0};
        Buffer result;
        HR(device->CreateBuffer(&desc, &initial, &result.buffer));
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
        view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements = 8;
        view.Buffer.Flags = append ? D3D11_BUFFER_UAV_FLAG_APPEND : 0;
        HR(device->CreateUnorderedAccessView(result.buffer.Get(), &view, &result.uav));
        return result;
    }

    void BindProducer(bool nullState = false) {
        context->ClearState();
        ID3D11ShaderResourceView* resources[] = {inputs[0].srv.Get(), inputs[1].srv.Get(), inputs[2].srv.Get()};
        ID3D11UnorderedAccessView* outputs[] = {results.uav.Get(), auxiliary[0].uav.Get(), auxiliary[1].uav.Get()};
        const UINT initialCounts[] = {2, UINT(-1), UINT(-1)};
        if (!nullState) {
            context->CSSetShader(producer.Get(), nullptr, 0);
            context->CSSetShaderResources(0, 3, resources);
            context->CSSetUnorderedAccessViews(0, 3, outputs, initialCounts);
            context->CSSetConstantBuffers(0, 1, producerConstants.GetAddressOf());
            context->CSSetConstantBuffers(1, 1, producerExtraConstants.GetAddressOf());
            context->CSSetSamplers(0, 1, producerSampler.GetAddressOf());
        }
        // Higher slots and graphics bindings belong to the external renderer.
        context->CSSetShaderResources(7, 1, &resources[0]);
        context->CSSetUnorderedAccessViews(7, 1, sentinel.uav.GetAddressOf(), nullptr);
        context->CSSetConstantBuffers(7, 1, producerConstants.GetAddressOf());
        context->CSSetSamplers(7, 1, producerSampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, &resources[0]);
        context->OMSetRenderTargets(1, graphicsOutput.rtv.GetAddressOf(), nullptr);
    }

    void BindWarp(bool capture) {
        context->CSSetShader(warp[capture].Get(), nullptr, 0);
        ID3D11ShaderResourceView* resources[] = {warpColor.srv.Get(), nullptr, warpDepth.srv.Get()};
        ID3D11UnorderedAccessView* outputs[] = {warpOutput[0].uav.Get(), warpOutput[1].uav.Get(), warpOutput[2].uav.Get()};
        context->CSSetShaderResources(0, 3, resources);
        context->CSSetUnorderedAccessViews(0, capture ? 3 : 1, outputs, nullptr);
        context->CSSetConstantBuffers(0, 1, warpConstants.GetAddressOf());
        context->CSSetConstantBuffers(1, 1, warpBlackoutConstants.GetAddressOf());
        context->CSSetSamplers(0, 1, warpSampler.GetAddressOf());
    }
    void DispatchAndUnbindWarp(bool capture) {
        context->Dispatch(1, 1, 1);
        // Exact former WarpFrame cleanup: bindings are cleared, not restored.
        ID3D11ShaderResourceView* resources[3]{};
        ID3D11UnorderedAccessView* outputs[3]{};
        context->CSSetShaderResources(0, 3, resources);
        context->CSSetUnorderedAccessViews(0, capture ? 3 : 1, outputs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }
    void Warp(bool capture, bool preserve) {
        if (preserve) {
            DapaComputeState saved(context.Get(), capture);
            BindWarp(capture);
            DispatchAndUnbindWarp(capture);
        } else {
            BindWarp(capture);
            DispatchAndUnbindWarp(capture);
        }
    }
    void EarlyReturn(bool capture) {
        DapaComputeState saved(context.Get(), capture);
        BindWarp(capture);
        return; // No explicit unbind: the guard must also handle an early exit.
    }

    void CheckBindings(bool nullState = false, ID3D11ComputeShader* overrideShader = nullptr,
        ID3D11ClassInstance* expectedInstance = nullptr, ID3D11Buffer* overrideConstants = nullptr,
        ID3D11Buffer* overrideExtraConstants = nullptr) {
        ComPtr<ID3D11ComputeShader> shader;
        ID3D11ClassInstance* instance = nullptr;
        UINT instanceCount = 1;
        context->CSGetShader(&shader, &instance, &instanceCount);
        if (!shader) instanceCount = 0;
        ComPtr<ID3D11ClassInstance> ownedInstance;
        ownedInstance.Attach(instance);
        Check(shader.Get() == (nullState ? nullptr : overrideShader ? overrideShader : producer.Get()), "compute shader not restored");
        Check(instanceCount == (expectedInstance ? 1u : 0u) && instance == expectedInstance, "shader class instances not restored");
        for (UINT slot = 0; slot < 3; ++slot) {
            ComPtr<ID3D11ShaderResourceView> resource;
            ComPtr<ID3D11UnorderedAccessView> output;
            context->CSGetShaderResources(slot, 1, &resource);
            context->CSGetUnorderedAccessViews(slot, 1, &output);
            Check(resource.Get() == (nullState ? nullptr : inputs[slot].srv.Get()), "compute input not restored");
            auto* wanted = slot == 0 ? results.uav.Get() : auxiliary[slot - 1].uav.Get();
            Check(output.Get() == (nullState ? nullptr : wanted), "compute output not restored");
        }
        ComPtr<ID3D11Buffer> constants;
        ComPtr<ID3D11SamplerState> sampler;
        context->CSGetConstantBuffers(0, 1, &constants);
        context->CSGetSamplers(0, 1, &sampler);
        Check(constants.Get() == (nullState ? nullptr : overrideConstants ? overrideConstants : producerConstants.Get()), "compute constant buffer not restored");
        ComPtr<ID3D11Buffer> extraConstants;
        context->CSGetConstantBuffers(1, 1, &extraConstants);
        Check(extraConstants.Get() == (nullState ? nullptr : overrideExtraConstants ? overrideExtraConstants : producerExtraConstants.Get()),
            "compute b1 constant buffer not restored");
        Check(sampler.Get() == (nullState ? nullptr : producerSampler.Get()), "compute sampler not restored");
        ComPtr<ID3D11ShaderResourceView> resource, pixelResource;
        ComPtr<ID3D11UnorderedAccessView> output;
        ComPtr<ID3D11RenderTargetView> target;
        constants.Reset(); sampler.Reset();
        context->CSGetShaderResources(7, 1, &resource);
        context->CSGetUnorderedAccessViews(7, 1, &output);
        context->CSGetConstantBuffers(7, 1, &constants);
        context->CSGetSamplers(7, 1, &sampler);
        context->PSGetShaderResources(0, 1, &pixelResource);
        context->OMGetRenderTargets(1, &target, nullptr);
        Check(resource == inputs[0].srv && output == sentinel.uav && constants == producerConstants
            && sampler == producerSampler, "higher compute slot changed");
        Check(pixelResource == inputs[0].srv && target == graphicsOutput.rtv, "graphics binding changed");
    }
    UINT Count() {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(UINT); desc.Usage = D3D11_USAGE_DEFAULT;
        ComPtr<ID3D11Buffer> counter, staging;
        HR(device->CreateBuffer(&desc, nullptr, &counter));
        context->CopyStructureCount(counter.Get(), 0, results.uav.Get());
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HR(device->CreateBuffer(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), counter.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const UINT value = *static_cast<const UINT*>(mapped.pData);
        context->Unmap(staging.Get(), 0);
        return value;
    }
    Pixel Read(ID3D11Buffer* source, UINT element) {
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
        ComPtr<ID3D11Buffer> staging;
        HR(device->CreateBuffer(&desc, nullptr, &staging));
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        Pixel value;
        std::memcpy(value.data(), static_cast<const char*>(mapped.pData) + element * sizeof(Pixel), sizeof(Pixel));
        context->Unmap(staging.Get(), 0);
        return value;
    }
    void CheckWarpOutput(bool capture) {
        D3D11_TEXTURE2D_DESC desc{};
        warpOutput[0].texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.BindFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        HR(device->CreateTexture2D(&desc, nullptr, &staging));
        for (int i = 0; i < (capture ? 3 : 1); ++i) {
            context->CopyResource(staging.Get(), warpOutput[i].texture.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const Pixel wanted = i == 2 ? Pixel{0, 0.5f, 0.5f, 1} : Pixel{0.25f, 0.5f, 0.75f, 1};
            for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 8; ++x) {
                Pixel actual;
                std::memcpy(actual.data(), static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(Pixel), sizeof(Pixel));
                Equal(actual, wanted, "production warp/capture output changed");
            }
            context->Unmap(staging.Get(), 0);
        }
    }
    void CheckDebug(bool negativeControl = false) {
        for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            HR(debug->GetMessage(i, nullptr, &size));
            std::vector<unsigned char> bytes(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            HR(debug->GetMessage(i, message, &size));
            if (negativeControl && message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                // Debug runtimes differ in whether a null-CS Dispatch warns.
                // The binding and missing GPU append are checked directly.
                std::printf("Negative control validation: %s\n", message->pDescription);
                continue;
            }
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D11: %s\n", message->pDescription);
                Check(false, "unexpected D3D11 validation warning/error");
            }
        }
        debug->ClearStoredMessages();
    }
    void ProducerResume(bool capture, bool preserve) {
        Trace(capture ? (preserve ? "capture preserved producer" : "capture old behavior producer")
            : (preserve ? "shipping preserved producer" : "shipping old behavior producer"));
        BindProducer();
        Trace("producer first dispatch");
        context->Dispatch(1, 1, 1);
        Trace("warp invocation");
        Warp(capture, preserve);
        Trace("warp returned");
        if (preserve) CheckBindings();
        else {
            CheckDebug(); // No warnings are allowed before the broken resume.
            ComPtr<ID3D11ComputeShader> missing;
            context->CSGetShader(&missing, nullptr, nullptr);
            Check(!missing, "negative control did not lose the compute shader");
        }
        // Deliberately no shader/resource/CB/sampler rebinding before resuming.
        context->Dispatch(1, 1, 1);
        Trace("producer resumed, read counter");
        Check(Count() == (preserve ? 4u : 3u), "producer did not resume at its preserved append counter");
        Trace("read producer output");
        Equal(Read(results.buffer.Get(), 2), expected, "initial producer output wrong");
        if (preserve) Equal(Read(results.buffer.Get(), 3), expected, "resumed producer output wrong");
        Equal(Read(auxiliary[0].buffer.Get(), 0), expected, "auxiliary UAV output wrong");
        Equal(Read(auxiliary[1].buffer.Get(), 0), expected, "third UAV output wrong");
        CheckWarpOutput(capture);
        Trace("check producer debug messages");
        CheckDebug(!preserve);
    }

    void ClassInstances() {
        Trace("class instances fixture");
        const char* source = R"HLSL(
interface IMode { float4 Apply(float4 color); };
class ScaleMode : IMode { float4 scale; float4 Apply(float4 color) { return color * scale; } };
IMode activeMode;
cbuffer Params : register(b0) { float4 color; };
AppendStructuredBuffer<float4> results : register(u0);
[numthreads(1,1,1)] void CSMain(uint3 tid : SV_DispatchThreadID) { results.Append(activeMode.Apply(color)); }
)HLSL";
        ComPtr<ID3D11ClassLinkage> linkage;
        Trace("class create linkage");
        HR(device->CreateClassLinkage(&linkage));
        Trace("class compile shader");
        auto linkedShader = Shader(source, nullptr, linkage.Get());
        ComPtr<ID3D11ClassInstance> instance;
        Trace("class create instance");
        HR(linkage->CreateClassInstance("ScaleMode", 0, 0, 0, 0, &instance));
        BindProducer();
        Trace("class bind shader/instance");
        context->CSSetShader(linkedShader.Get(), instance.GetAddressOf(), 1);
        Trace("class first dispatch (before DAPA guard)");
        context->Dispatch(1, 1, 1);
        Trace("class first readback (before DAPA guard)");
        const Pixel before = Read(results.buffer.Get(), 2);
        Trace("class guarded warp");
        Warp(true, true);
        Trace("class check restored bindings");
        CheckBindings(false, linkedShader.Get(), instance.Get());
        Trace("class second dispatch");
        context->Dispatch(1, 1, 1);
        Trace("class final counter/readback");
        Check(Count() == 4, "linked shader did not resume");
        Equal(Read(results.buffer.Get(), 3), before, "class-linked shader output changed");
        CheckDebug();
    }
    void Predication() {
        Trace("suppressing predicate fixture");
        BindProducer();
        ComPtr<ID3D11Predicate> predicate;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION_PREDICATE,0};
        HR(device->CreatePredicate(&desc,&predicate));
        context->Begin(predicate.Get());context->End(predicate.Get());context->Flush();
        BOOL visible=TRUE;HRESULT ready=S_FALSE;
        for(unsigned i=0;i<10000&&ready==S_FALSE;++i) {
            ready=context->GetData(predicate.Get(),&visible,sizeof(visible),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(ready==S_FALSE)Sleep(1);
        }
        Check(ready==S_OK&&visible==FALSE,"empty predicate did not resolve false");
        auto restored=[&](ID3D11Predicate* wanted,BOOL value) {
            ComPtr<ID3D11Predicate> actual;BOOL actualValue=FALSE;
            context->GetPredication(&actual,&actualValue);
            Check(actual.Get()==wanted&&actualValue==value,"predicate pointer/value not restored");
        };
        // Predication suppresses work when query data equals the supplied value.
        context->SetPredication(predicate.Get(),FALSE);context->Dispatch(1,1,1);
        context->SetPredication(nullptr,FALSE);
        Check(Count()==2,"predicate negative control did not suppress Dispatch");
        for(bool capture:{false,true}) {
            BindProducer();context->SetPredication(predicate.Get(),FALSE);
            Warp(capture,true);restored(predicate.Get(),FALSE);
            context->Dispatch(1,1,1); // Restored predicate must suppress producer again.
            context->SetPredication(nullptr,FALSE);
            Check(Count()==2,"warp lost the suppressing predicate on return");
            CheckWarpOutput(capture);
            CheckDebug();
        }
        // The lightweight guard covers both cache and swapchain-copy commands.
        const std::array<Pixel,64> zero{};
        auto destination=MakeTexture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,
            D3D11_BIND_SHADER_RESOURCE,zero.data(),8*sizeof(Pixel));
        auto readFirst=[&]() {
            D3D11_TEXTURE2D_DESC td{};destination.texture->GetDesc(&td);
            td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&td,nullptr,&staging));
            context->CopyResource(staging.Get(),destination.texture.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));Pixel value;
            std::memcpy(value.data(),mapped.pData,sizeof(value));context->Unmap(staging.Get(),0);return value;
        };
        for(bool whole:{false,true}) {
            context->UpdateSubresource(destination.texture.Get(),0,nullptr,zero.data(),8*sizeof(Pixel),0);
            auto copy=[&]() {
                if(whole)context->CopyResource(destination.texture.Get(),warpColor.texture.Get());
                else context->CopySubresourceRegion(destination.texture.Get(),0,0,0,0,warpColor.texture.Get(),0,nullptr);
            };
            context->SetPredication(predicate.Get(),FALSE);copy();context->SetPredication(nullptr,FALSE);
            Equal(readFirst(),Pixel{},"predicate negative control did not suppress Copy");
            context->SetPredication(predicate.Get(),FALSE);
            {DapaPredicationState unpredicated(context.Get());copy();}
            restored(predicate.Get(),FALSE);context->SetPredication(nullptr,FALSE);
            Equal(readFirst(),Pixel{.25f,.5f,.75f,1},"guarded copy remained suppressed");
        }
        // Even a null predicate retains a caller-visible BOOL value.
        context->SetPredication(nullptr,TRUE);
        {DapaPredicationState unpredicated(context.Get());}
        restored(nullptr,TRUE);context->SetPredication(nullptr,FALSE);
        CheckDebug();
        std::puts("Predication: verified suppressed Dispatch/Copy controls, warp/copy execution and exact restoration PASS");
    }
    void ConstantBufferRange() {
        Trace("constant buffer subrange fixture");
        ComPtr<ID3D11DeviceContext1> context1;
        HR(context.As(&context1));
        std::array<Pixel, 32> values{};
        values[16] = Pixel{2, 3, 0, 0};
        auto rangedBuffer = ConstantBuffer(values.data(), sizeof(values));
        std::array<Pixel, 64> extraValues{};
        extraValues[32] = Pixel{5, 7, 11, 13};
        auto rangedExtraBuffer = ConstantBuffer(extraValues.data(), sizeof(extraValues));
        ID3D11Buffer* buffers[]{rangedBuffer.Get(), rangedExtraBuffer.Get()};
        const UINT first[]{16, 32}, count[]{16, 16};
        for (bool capture : {false, true}) {
            BindProducer();
            context1->CSSetConstantBuffers1(0, 2, buffers, first, count);
            context->Dispatch(1, 1, 1);
            Warp(capture, true);
            CheckBindings(false, nullptr, nullptr, rangedBuffer.Get(), rangedExtraBuffer.Get());
            for (UINT slot=0;slot<2;++slot) {
                ComPtr<ID3D11Buffer> actual;
                UINT actualFirst=0, actualCount=0;
                context1->CSGetConstantBuffers1(slot, 1, &actual, &actualFirst, &actualCount);
                Check(actual.Get()==buffers[slot] && actualFirst==first[slot] && actualCount==count[slot],
                    "D3D11.1 b0/b1 constant-buffer range lost");
            }
            // Retaining the buffer pointer alone would read the zeros at offset 0.
            context->Dispatch(1, 1, 1);
            Check(Count() == 4, "producer did not resume with constant-buffer range");
            Equal(Read(results.buffer.Get(), 2), expected, "initial ranged producer output wrong");
            Equal(Read(results.buffer.Get(), 3), expected, "restored constant-buffer range output wrong");
            CheckDebug();
        }
    }
};

static void Run(D3D_DRIVER_TYPE driver, bool skipHardwareClassLinkage) {
    Trace(driver == D3D_DRIVER_TYPE_WARP ? "begin WARP" : "begin hardware");
    Fixture fixture(driver);
    for (bool capture : {false, true}) {
        fixture.ProducerResume(capture, false); // old behavior must fail to resume
        fixture.ProducerResume(capture, true);
        Trace("null state fixture");
        fixture.BindProducer(true);
        fixture.Warp(capture, true);
        fixture.CheckBindings(true);
        fixture.CheckDebug();
        Trace("early return fixture");
        fixture.BindProducer();
        fixture.EarlyReturn(capture);
        fixture.CheckBindings();
        fixture.context->Dispatch(1, 1, 1);
        Check(fixture.Count() == 3, "early-return restoration changed append counter");
        Equal(fixture.Read(fixture.results.buffer.Get(), 2), Fixture::expected, "producer failed after early return");
        fixture.CheckDebug();
    }
    const bool skipClasses = driver == D3D_DRIVER_TYPE_HARDWARE && skipHardwareClassLinkage;
    if (skipClasses) {
        std::printf("SKIP hardware dynamic class linkage by explicit flag: local driver crashed during the baseline class-linked Dispatch/readback before DAPA; WARP still exercises class restoration\n");
        std::fflush(stdout);
    } else fixture.ClassInstances();
    fixture.ConstantBufferRange();
    fixture.Predication();
    fixture.context->ClearState();
    fixture.context->Flush();
    std::printf("PASS %s: production warp/capture, resumed producer, append counters, null state, early return, CB subranges, untouched higher/graphics slots; old behavior rejected; class instances=%s\n",
        driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware", skipClasses ? "SKIPPED" : "passed");
}
int main(int argc, char** argv) {
    try {
        bool skipHardwareClassLinkage = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
            else if (std::strcmp(argv[i], "--skip-hardware-class-linkage") == 0) skipHardwareClassLinkage = true;
            else throw std::runtime_error("usage: DapaComputeStateTests [--verbose] [--skip-hardware-class-linkage]");
        }
        Run(D3D_DRIVER_TYPE_WARP, false);
        Run(D3D_DRIVER_TYPE_HARDWARE, skipHardwareClassLinkage);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
