#include "OpenOVR/Compositor/DepthExtract.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Note(const char* message) { std::printf("DEPTH CHECKPOINT: %s\n", message); std::fflush(stdout); }
static void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void HR(HRESULT result) {
    if (FAILED(result)) {
        char message[64]; std::snprintf(message, sizeof(message), "D3D11 HRESULT 0x%08X", unsigned(result));
        throw std::runtime_error(message);
    }
}
static ComPtr<ID3DBlob> Compile(const char* text, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(text, std::strlen(text), nullptr, nullptr, nullptr,
        entry, profile, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors)
        throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
    HR(result); return code;
}
static ComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT bindings)
{
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = bindings;
    ComPtr<ID3D11Texture2D> result; HR(device->CreateTexture2D(&desc, nullptr, &result)); return result;
}
static std::vector<float> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture)
{
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<float> values(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(values.data() + y * desc.Width, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * sizeof(float));
    context->Unmap(staging.Get(), 0); return values;
}
static void CheckValues(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, float expected, const char* message)
{
    for (float value : Read(device, context, texture)) Check(std::abs(value - expected) <= 0.000001f, message);
}
static UINT Counter(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11UnorderedAccessView* view)
{
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = 4;
    ComPtr<ID3D11Buffer> buffer; HR(device->CreateBuffer(&desc, nullptr, &buffer));
    context->CopyStructureCount(buffer.Get(), 0, view);
    desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging; HR(device->CreateBuffer(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), buffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    const UINT value = *static_cast<const UINT*>(mapped.pData); context->Unmap(staging.Get(), 0); return value;
}
static void CheckDebug(ID3D11InfoQueue* queue)
{
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; HR(queue->GetMessage(i, nullptr, &size));
        std::vector<unsigned char> bytes(size); auto* message = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        HR(queue->GetMessage(i, message, &size));
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
            std::printf("D3D11: %s\n", message->pDescription);
            Check(false, "Production extraction emitted a D3D11 warning/error");
        }
    }
    queue->ClearStoredMessages();
}

// Negative control: the former dispatch sequence restored selected CS objects,
// but never detached an overlapping writable depth output or saved instances.
static void OldDispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader,
    ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* output, UINT width, UINT height)
{
    ComPtr<ID3D11ComputeShader> oldShader; ComPtr<ID3D11ShaderResourceView> oldSource;
    ComPtr<ID3D11UnorderedAccessView> oldOutput;
    context->CSGetShader(&oldShader, nullptr, nullptr);
    context->CSGetShaderResources(0, 1, &oldSource);
    context->CSGetUnorderedAccessViews(0, 1, &oldOutput);
    context->CSSetShader(shader, nullptr, 0);
    context->CSSetShaderResources(0, 1, &source);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    context->CSSetShader(oldShader.Get(), nullptr, 0);
    auto* previousSource = oldSource.Get(); context->CSSetShaderResources(0, 1, &previousSource);
    auto* previousOutput = oldOutput.Get(); context->CSSetUnorderedAccessViews(0, 1, &previousOutput, nullptr);
}

static void Run(D3D_DRIVER_TYPE driver, bool skipHardwareClassLinkage)
{
    Note(driver == D3D_DRIVER_TYPE_WARP ? "BEGIN WARP" : "BEGIN hardware");
    const bool linkedProducer = driver != D3D_DRIVER_TYPE_HARDWARE || !skipHardwareClassLinkage;
    if (!linkedProducer)
        Note("EXPLICIT SKIP: hardware class linkage only; standalone linked producer crashes this driver before extraction. All hardware depth/OM cases still run with ordinary CB producer; WARP covers class linkage.");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<ID3D11InfoQueue> debug; HR(device.As(&debug));
    const auto code = Compile(ocu_depth_extract::Shader, "CS_DepthExtract", "cs_5_0");
    ComPtr<ID3D11ComputeShader> extraction;
    HR(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &extraction));

    // A live game compute shader with a dynamic class instance must survive the
    // extraction, not merely its shader COM pointer. Dispatch it afterwards.
    const char* gameCode = R"(
interface IValue { float Get(); };
class StoredValue : IValue { float4 value; float Get() { return value.x; } };
IValue selected;
RWTexture2D<float> destination : register(u0);
[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { destination[id.xy] = selected.Get(); }
)";
    const char* ordinaryGameCode = R"(
cbuffer Params : register(b0) { float4 value; };
RWTexture2D<float> destination : register(u0);
[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) { destination[id.xy] = value.x; }
)";
    auto gameBytes = Compile(linkedProducer ? gameCode : ordinaryGameCode, "main", "cs_5_0");
    ComPtr<ID3D11ClassLinkage> linkage;
    if (linkedProducer) {
        Note("dynamic shader compiled; creating linkage");
        HR(device->CreateClassLinkage(&linkage));
    }
    ComPtr<ID3D11ComputeShader> gameShader;
    Note(linkedProducer ? "creating dynamic shader" : "creating ordinary CB shader");
    HR(device->CreateComputeShader(gameBytes->GetBufferPointer(), gameBytes->GetBufferSize(), linkage.Get(), &gameShader));
    ComPtr<ID3D11ClassInstance> instance;
    if (linkedProducer) {
        Note("creating dynamic class instance");
        HR(linkage->CreateClassInstance("StoredValue", 0, 0, 0, 0, &instance));
    }
    // Data-dependent class access prevents constant folding from eliminating
    // the interface slot, which would make a one-instance binding invalid.
    D3D11_BUFFER_DESC constantsDesc{}; constantsDesc.ByteWidth = 16;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const float classData[4] = {7, 0, 0, 0};
    const D3D11_SUBRESOURCE_DATA constantsData{classData, 0, 0};
    ComPtr<ID3D11Buffer> constants;
    HR(device->CreateBuffer(&constantsDesc, &constantsData, &constants));

    // Prove the driver's linked producer works by itself before any production
    // extraction/state capture runs. Flushed checkpoints localize driver faults.
    {
        auto baseline = Texture(device.Get(), 1, 1, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> baselineOutput;
        HR(device->CreateUnorderedAccessView(baseline.Get(), nullptr, &baselineOutput));
        Note(linkedProducer ? "binding dynamic producer before any extraction" : "binding ordinary producer before any extraction");
        auto* baselineInstance = instance.Get(); context->CSSetShader(gameShader.Get(), &baselineInstance, linkedProducer ? 1 : 0);
        auto* baselineConstants = constants.Get(); context->CSSetConstantBuffers(0, 1, &baselineConstants);
        auto* baselineView = baselineOutput.Get(); context->CSSetUnorderedAccessViews(0, 1, &baselineView, nullptr);
        CheckDebug(debug.Get());
        Note(linkedProducer ? "dispatching dynamic producer before any extraction" : "dispatching ordinary producer before any extraction");
        context->Dispatch(1, 1, 1);
        Note(linkedProducer ? "reading dynamic producer before any extraction" : "reading ordinary producer before any extraction");
        CheckValues(device.Get(), context.Get(), baseline.Get(), 7, "Producer failed before any extraction");
        CheckDebug(debug.Get()); context->ClearState();
        Note("standalone producer passed; beginning production depth cases");
    }

    unsigned cases = 0;
    for (bool d24 : {false, true}) for (UINT width : {8u, 13u}) {
        constexpr UINT height = 7;
        auto depth = Texture(device.Get(), width, height, d24 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS,
            D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        auto otherDepth = Texture(device.Get(), width, height, d24 ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_R32_TYPELESS,
            D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE);
        D3D11_DEPTH_STENCIL_VIEW_DESC depthView{};
        depthView.Format = d24 ? DXGI_FORMAT_D24_UNORM_S8_UINT : DXGI_FORMAT_D32_FLOAT;
        depthView.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> writable, readOnly, other;
        HR(device->CreateDepthStencilView(depth.Get(), &depthView, &writable));
        HR(device->CreateDepthStencilView(otherDepth.Get(), &depthView, &other));
        depthView.Flags = D3D11_DSV_READ_ONLY_DEPTH;
        if (d24) depthView.Flags |= D3D11_DSV_READ_ONLY_STENCIL;
        HR(device->CreateDepthStencilView(depth.Get(), &depthView, &readOnly));
        context->ClearDepthStencilView(writable.Get(), D3D11_CLEAR_DEPTH, .375f, 0);
        D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
        sourceView.Format = d24 ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
        sourceView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sourceView.Texture2D.MipLevels = 1;
        ComPtr<ID3D11ShaderResourceView> source; HR(device->CreateShaderResourceView(depth.Get(), &sourceView, &source));
        auto outputTexture = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> output; HR(device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &output));
        auto targetTexture = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> target; HR(device->CreateRenderTargetView(targetTexture.Get(), nullptr, &target));
        const float color[4] = {.625f, 0, 0, 0}; context->ClearRenderTargetView(target.Get(), color);
        auto gameTexture = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11UnorderedAccessView> gameOutput; HR(device->CreateUnorderedAccessView(gameTexture.Get(), nullptr, &gameOutput));
        auto gameInputTexture = Texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE);
        ComPtr<ID3D11ShaderResourceView> gameInput; HR(device->CreateShaderResourceView(gameInputTexture.Get(), nullptr, &gameInput));
        D3D11_BUFFER_DESC appendDesc{}; appendDesc.ByteWidth = 64; appendDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        appendDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; appendDesc.StructureByteStride = 4;
        ComPtr<ID3D11Buffer> append; HR(device->CreateBuffer(&appendDesc, nullptr, &append));
        D3D11_UNORDERED_ACCESS_VIEW_DESC appendView{}; appendView.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        appendView.Buffer.NumElements = 16; appendView.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
        ComPtr<ID3D11UnorderedAccessView> sentinel; HR(device->CreateUnorderedAccessView(append.Get(), &appendView, &sentinel));

        // The old path reports success yet writes zero because the bound DSV
        // makes D3D11 reject the depth SRV. Its expected warning is isolated.
        context->ClearState(); context->OMSetRenderTargets(0, nullptr, writable.Get());
        OldDispatch(context.Get(), extraction.Get(), source.Get(), output.Get(), width, height);
        CheckValues(device.Get(), context.Get(), outputTexture.Get(), 0, "Old sequence did not reproduce zero depth");
        Check(debug->GetNumStoredMessagesAllowedByRetrievalFilter() > 0, "Expected old input/output hazard warning");
        debug->ClearStoredMessages(); context->ClearState();

        // Exercise writable same-resource, matching read-only, unrelated, and
        // absent DSVs, each with sparse RTV bindings and a live OM UAV counter.
        for (int mode = 0; mode != 4; ++mode) for (UINT targetSlot : {0u, 2u}) {
            ID3D11DepthStencilView* boundDepth = mode == 0 ? writable.Get() : mode == 1 ? readOnly.Get() : mode == 2 ? other.Get() : nullptr;
            ID3D11RenderTargetView* targets[3]{}; targets[targetSlot] = target.Get();
            auto* sentinelView = sentinel.Get(); const UINT initialCounter = 3;
            context->OMSetRenderTargetsAndUnorderedAccessViews(targetSlot + 1, targets, boundDepth,
                targetSlot + 1, 1, &sentinelView, &initialCounter);
            auto* gameInstance = instance.Get(); context->CSSetShader(gameShader.Get(), &gameInstance, linkedProducer ? 1 : 0);
            auto* gameConstants = constants.Get(); context->CSSetConstantBuffers(0, 1, &gameConstants);
            auto* gameSource = gameInput.Get(); context->CSSetShaderResources(0, 1, &gameSource);
            auto* gameDestination = gameOutput.Get(); context->CSSetUnorderedAccessViews(0, 1, &gameDestination, nullptr);
            CheckDebug(debug.Get());
            Check(ocu_depth_extract::Dispatch(context.Get(), extraction.Get(), source.Get(), output.Get(), width, height), "Production extraction rejected valid depth");
            CheckValues(device.Get(), context.Get(), outputTexture.Get(), .375f, "Extracted depth differs from actual bound scene depth");
            CheckValues(device.Get(), context.Get(), targetTexture.Get(), .625f, "Extraction changed game scene color");
            ComPtr<ID3D11DepthStencilView> afterDepth; ID3D11RenderTargetView* afterTargets[8]{};
            context->OMGetRenderTargets(8, afterTargets, &afterDepth);
            Check(afterDepth.Get() == boundDepth, "Game DSV binding changed");
            for (UINT i = 0; i != 8; ++i) {
                Check(afterTargets[i] == (i == targetSlot ? target.Get() : nullptr), "Game RTV binding changed");
                if (afterTargets[i]) afterTargets[i]->Release();
            }
            ComPtr<ID3D11UnorderedAccessView> afterSentinel;
            context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, targetSlot + 1, 1, &afterSentinel);
            Check(afterSentinel == sentinel && Counter(device.Get(), context.Get(), sentinel.Get()) == initialCounter, "Game OM UAV or append counter changed");
            ComPtr<ID3D11ComputeShader> afterShader; ID3D11ClassInstance* afterInstances[2]{}; UINT instanceCount = 2;
            context->CSGetShader(&afterShader, afterInstances, &instanceCount);
            Check(afterShader == gameShader && instanceCount == (linkedProducer ? 1u : 0u) &&
                (!linkedProducer || afterInstances[0] == instance.Get()), "Game CS shader or dynamic class instance changed");
            for (UINT i = 0; i < instanceCount; ++i) if (afterInstances[i]) afterInstances[i]->Release();
            ComPtr<ID3D11ShaderResourceView> afterSource; context->CSGetShaderResources(0, 1, &afterSource);
            ComPtr<ID3D11UnorderedAccessView> afterOutput; context->CSGetUnorderedAccessViews(0, 1, &afterOutput);
            Check(afterSource == gameInput && afterOutput == gameOutput, "Game CS resources changed");
            context->Dispatch(width, height, 1);
            CheckValues(device.Get(), context.Get(), gameTexture.Get(), 7, "Game compute draw no longer works after extraction");
            CheckDebug(debug.Get()); context->ClearState(); ++cases;
        }
    }
    Check(!ocu_depth_extract::Dispatch(nullptr, extraction.Get(), nullptr, nullptr, 0, 0), "Invalid extraction must be rejected");
    context->ClearState(); context->Flush();
    std::printf("PASS %s: %u depth/state cases; old writable-DSV zero-depth reproduced; production depth, color, OM UAV/counter and CS state preserved; class linkage=%s\n",
        driver == D3D_DRIVER_TYPE_WARP ? "WARP" : "hardware", cases, linkedProducer ? "tested" : "explicit hardware-only skip");
    std::fflush(stdout);
}

int main(int argc, char** argv) try {
    const bool skip = argc == 2 && std::strcmp(argv[1], "--skip-hardware-class-linkage") == 0;
    Check(argc == 1 || skip, "Usage: OCUDepthExtractTest [--skip-hardware-class-linkage]");
    Run(D3D_DRIVER_TYPE_WARP, skip); Run(D3D_DRIVER_TYPE_HARDWARE, skip); return 0;
}
catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
