#include "OpenOVR/Compositor/FoveationBlackoutRenderer.h"
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using ocu_foveation::BlackoutFrame;
static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT result) { Check(SUCCEEDED(result), "D3D11 operation failed"); }

struct Image {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
};

static Image MakeImage(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format)
{
    Image image;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = format; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HR(device->CreateTexture2D(&desc, nullptr, &image.texture));
    HR(device->CreateRenderTargetView(image.texture.Get(), nullptr, &image.rtv));
    HR(device->CreateShaderResourceView(image.texture.Get(), nullptr, &image.srv));
    return image;
}

static std::vector<unsigned char> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source)
{
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    Check(desc.SampleDesc.Count == 1, "readback is single sampled");
    desc.BindFlags = desc.MiscFlags = 0; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    std::vector<unsigned char> result(desc.Width * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(result.data() + y * desc.Width * 4,
            static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return result;
}

// Snapshot every public graphics/compute binding family, including D3D11.1
// constant-buffer ranges and high UAV slots. References returned by Get calls
// stay alive until comparison is complete.
struct Pipeline {
    std::vector<ComPtr<IUnknown>> objects;
    std::vector<unsigned char> values;
    template<class T> void Keep(T* pointer) { ComPtr<IUnknown> item; item.Attach(pointer); objects.push_back(std::move(item)); }
    template<class T> void Value(const T& value) {
        const auto* begin = reinterpret_cast<const unsigned char*>(&value);
        values.insert(values.end(), begin, begin + sizeof(value));
    }
    template<class T, size_t N> void Keep(std::array<T*, N>& items) { for (auto* item : items) Keep(item); }
    Pipeline(ID3D11DeviceContext* context, UINT uavSlots) {
        ComPtr<ID3D11DeviceContext1> context1; HR(context->QueryInterface(IID_PPV_ARGS(&context1)));
#define STAGE(prefix, type) do { \
        type* shader = nullptr; std::array<ID3D11ClassInstance*, D3D11_SHADER_MAX_INTERFACES> instances{}; \
        UINT count = static_cast<UINT>(instances.size()); context->prefix##GetShader(&shader, instances.data(), &count); \
        Keep(shader); Value(count); for (UINT i = 0; i < count; ++i) Keep(instances[i]); \
        std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{}; \
        std::array<UINT, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> first{}, number{}; \
        context1->prefix##GetConstantBuffers1(0, static_cast<UINT>(buffers.size()), buffers.data(), first.data(), number.data()); \
        Keep(buffers); Value(first); Value(number); \
        std::array<ID3D11ShaderResourceView*, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{}; \
        context->prefix##GetShaderResources(0, static_cast<UINT>(resources.size()), resources.data()); Keep(resources); \
        std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers{}; \
        context->prefix##GetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data()); Keep(samplers); \
    } while (false)
        STAGE(VS, ID3D11VertexShader); STAGE(HS, ID3D11HullShader); STAGE(DS, ID3D11DomainShader);
        STAGE(GS, ID3D11GeometryShader); STAGE(PS, ID3D11PixelShader); STAGE(CS, ID3D11ComputeShader);
#undef STAGE
        ID3D11InputLayout* layout = nullptr; context->IAGetInputLayout(&layout); Keep(layout);
        D3D11_PRIMITIVE_TOPOLOGY topology{}; context->IAGetPrimitiveTopology(&topology); Value(topology);
        std::array<ID3D11Buffer*, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> vertex{};
        std::array<UINT, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> strides{}, offsets{};
        context->IAGetVertexBuffers(0, static_cast<UINT>(vertex.size()), vertex.data(), strides.data(), offsets.data());
        Keep(vertex); Value(strides); Value(offsets);
        ID3D11Buffer* index = nullptr; DXGI_FORMAT format{}; UINT offset{};
        context->IAGetIndexBuffer(&index, &format, &offset); Keep(index); Value(format); Value(offset);
        std::array<ID3D11Buffer*, D3D11_SO_BUFFER_SLOT_COUNT> stream{};
        context->SOGetTargets(static_cast<UINT>(stream.size()), stream.data()); Keep(stream);
        ID3D11RasterizerState* raster = nullptr; context->RSGetState(&raster); Keep(raster);
        std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
        UINT count = static_cast<UINT>(viewports.size()); context->RSGetViewports(&count, viewports.data());
        Value(count); for (UINT i = 0; i < count; ++i) Value(viewports[i]);
        std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
        count = static_cast<UINT>(scissors.size()); context->RSGetScissorRects(&count, scissors.data());
        Value(count); for (UINT i = 0; i < count; ++i) Value(scissors[i]);
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        ID3D11DepthStencilView* depth = nullptr;
        context->OMGetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), &depth); Keep(targets); Keep(depth);
        ID3D11BlendState* blend = nullptr; std::array<FLOAT, 4> factors{}; UINT mask{};
        context->OMGetBlendState(&blend, factors.data(), &mask); Keep(blend); Value(factors); Value(mask);
        ID3D11DepthStencilState* depthState = nullptr; UINT reference{};
        context->OMGetDepthStencilState(&depthState, &reference); Keep(depthState); Value(reference);
        std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> uavs{};
        context->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, uavSlots, uavs.data());
        for (UINT i = 0; i < uavSlots; ++i) Keep(uavs[i]);
        context->CSGetUnorderedAccessViews(0, uavSlots, uavs.data());
        for (UINT i = 0; i < uavSlots; ++i) Keep(uavs[i]);
        ID3D11Predicate* predicate = nullptr; BOOL predicateValue{};
        context->GetPredication(&predicate, &predicateValue); Keep(predicate); Value(predicateValue);
    }
    void Equal(const Pipeline& other) const {
        Check(values == other.values, "all pipeline scalar values and constant ranges restored");
        Check(objects.size() == other.objects.size(), "pipeline snapshot shape restored");
        for (size_t i = 0; i < objects.size(); ++i)
            Check(objects[i].Get() == other.objects[i].Get(), "pipeline resource or shader binding restored");
    }
};

static ComPtr<ID3DBlob> Compile(const char* source, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    HRESULT result = D3DCompile(source, std::strlen(source), "BlackoutTestProducer", nullptr, nullptr,
        entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(result) && errors) std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
    HR(result); return code;
}

struct Fixture {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1; ComPtr<ID3D11InfoQueue> messages;
    FoveationBlackoutRenderer renderer;
    UINT uavSlots = 8;
    bool Start(D3D_DRIVER_TYPE driver, D3D_FEATURE_LEVEL level) {
        HRESULT result = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(result)) return false;
        HR(context.As(&context1)); HR(device.As(&messages));
        uavSlots = level >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : 8;
        Check(renderer.Initialize(device.Get()), "initialize complete renderer");
        Check(renderer.Initialize(device.Get()), "same-device initialization is reusable");
        messages->ClearStoredMessages(); return true;
    }
    void DebugClean() {
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T length{}; HR(messages->GetMessage(i, nullptr, &length));
            std::vector<unsigned char> bytes(length); auto* item = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
            HR(messages->GetMessage(i, item, &length));
            if (item->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D11: %s\n", item->pDescription);
                Check(false, "debug layer must contain no warning/error");
            }
        }
    }
};

static void PixelTests(Fixture& test)
{
    BlackoutFrame frame; frame.frameId = 17;
    frame.centers[0][0] = .3125f; frame.centers[0][1] = .625f;
    frame.centers[1][0] = .875f; frame.centers[1][1] = .125f;
    frame.inner = .25f; frame.middle = .625f;
    constexpr UINT width = 80, height = 64;
    for (auto format : { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM }) {
        auto image = MakeImage(test.device.Get(), width, height, format);
        std::vector<unsigned char> initial(width * height * 4);
        for (size_t p = 0; p < initial.size(); ++p) initial[p] = static_cast<unsigned char>((p * 37 + 23) & 255);
        for (auto viewport : { D3D11_VIEWPORT{0, 0, 80, 64, 0, 1}, D3D11_VIEWPORT{8, 16, 64, 32, 0, 1} })
            for (float scale : {.5f, 1.f, 2.f}) for (int eye : {0, 1}) for (bool flip : {false, true})
                for (UINT flags = 0; flags != 8; ++flags) {
                    frame.horizontalScale = scale;
                    frame.mask.middle = (flags & 1) != 0; frame.mask.outer = (flags & 2) != 0;
                    frame.mask.cutoff = (flags & 4) != 0;
                    // One cutoff collapses onto middle; the other remains separate.
                    frame.mask.cutoffRadius = (eye == 0) ? .1f : .875f;
                    test.context->UpdateSubresource(image.texture.Get(), 0, nullptr, initial.data(), width * 4, 0);
                    Check(test.renderer.Apply(test.context.Get(), image.rtv.Get(), frame, eye, viewport, flip), "mask apply succeeds");
                    auto actual = Read(test.device.Get(), test.context.Get(), image.texture.Get());
                    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                        bool hidden = false;
                        if (x + .5 >= viewport.TopLeftX && x + .5 < viewport.TopLeftX + viewport.Width &&
                            y + .5 >= viewport.TopLeftY && y + .5 < viewport.TopLeftY + viewport.Height) {
                            double u = (x + .5 - viewport.TopLeftX) / viewport.Width;
                            double v = (y + .5 - viewport.TopLeftY) / viewport.Height;
                            if (flip) v = 1 - v;
                            double dx = 2 * (u - frame.centers[eye][0]) / scale;
                            double dy = 2 * (v - frame.centers[eye][1]);
                            double radius = std::hypot(dx, dy);
                            hidden = (frame.mask.middle && radius > frame.inner && radius <= frame.middle) ||
                                (frame.mask.outer && radius > frame.middle) ||
                                (frame.mask.cutoff && radius > ocu_foveation::BlackoutCutoff(frame.mask, frame.middle));
                        }
                        const size_t at = (y * width + x) * 4;
                        const unsigned char black[4]{0, 0, 0, 255};
                        Check(std::memcmp(actual.data() + at, hidden ? black : initial.data() + at, 4) == 0,
                            "GPU matches region unions and keeps every visible/outside-viewport byte");
                    }
                }
    }
    auto image = MakeImage(test.device.Get(), width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    const float colour[4]{.25f, .5f, .75f, 1}; test.context->ClearRenderTargetView(image.rtv.Get(), colour);
    auto initial = Read(test.device.Get(), test.context.Get(), image.texture.Get());
    D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    frame.mask.outer = true;
    auto invalid = frame; invalid.centers[0][0] = std::numeric_limits<float>::quiet_NaN();
    Check(!test.renderer.Apply(test.context.Get(), image.rtv.Get(), invalid, 0, viewport), "invalid geometry rejected");
    Check(!test.renderer.Apply(test.context.Get(), image.rtv.Get(), frame, 2, viewport), "invalid eye rejected");
    auto badViewport = viewport; badViewport.Width += 1;
    Check(!test.renderer.Apply(test.context.Get(), image.rtv.Get(), frame, 0, badViewport), "out-of-target viewport rejected");
    badViewport = viewport; badViewport.Height = std::numeric_limits<float>::infinity();
    Check(!test.renderer.Apply(test.context.Get(), image.rtv.Get(), frame, 0, badViewport), "nonfinite viewport rejected");
    ComPtr<ID3D11DeviceContext> deferred; HR(test.device->CreateDeferredContext(0, &deferred));
    Check(!test.renderer.Apply(deferred.Get(), image.rtv.Get(), frame, 0, viewport), "deferred context rejected");
    FoveationBlackoutRenderer cold;
    Check(!cold.Apply(test.context.Get(), image.rtv.Get(), frame, 0, viewport), "uninitialized renderer rejected");
    frame.frameId = 0;
    Check(test.renderer.Apply(test.context.Get(), image.rtv.Get(), frame, 0, viewport), "unlatched frame is a no-op");
    Check(Read(test.device.Get(), test.context.Get(), image.texture.Get()) == initial, "rejected/no-op calls never write target");

    // Deliberately put pixel centers exactly on the two boundaries; strict
    // inner exclusion and inclusive middle ownership must agree at equality.
    auto boundary = MakeImage(test.device.Get(), 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM);
    BlackoutFrame exact; exact.frameId = 23; exact.inner = .25f; exact.middle = .5f;
    exact.centers[0][0] = exact.centers[0][1] = 8.5f / 16;
    D3D11_VIEWPORT square{0, 0, 16, 16, 0, 1};
    const float white[4]{1, 1, 1, 1};
    for (bool middle : {false, true}) {
        test.context->ClearRenderTargetView(boundary.rtv.Get(), white);
        exact.mask.middle = middle; exact.mask.outer = !middle;
        Check(test.renderer.Apply(test.context.Get(), boundary.rtv.Get(), exact, 0, square), "exact-boundary mask applies");
        auto pixels = Read(test.device.Get(), test.context.Get(), boundary.texture.Get());
        Check(pixels[(8 * 16 + 10) * 4] == 255, "inner boundary is always visible");
        Check(pixels[(8 * 16 + 12) * 4] == (middle ? 0 : 255), "middle boundary belongs only to middle mask");
        Check(pixels[(8 * 16 + 13) * 4] == (middle ? 255 : 0), "outside-middle belongs only to outer mask");
    }
}

static void StateTests(Fixture& test)
{
    auto target = MakeImage(test.device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
    auto producer = MakeImage(test.device.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM);
    const float colour[4]{.25f, .5f, .75f, .5f};
    test.context->ClearRenderTargetView(target.rtv.Get(), colour);
    test.context->ClearRenderTargetView(producer.rtv.Get(), colour);
    auto producerBefore = Read(test.device.Get(), test.context.Get(), producer.texture.Get());
    constexpr char shader[] = R"HLSL(
struct P {float4 p:SV_Position;};
P VS(float4 p:POSITION){P o;o.p=p;return o;}
float4 PS(P p):SV_Target{return p.p;}
[maxvertexcount(3)] void GS(triangle P p[3],inout TriangleStream<P> stream){for(int i=0;i<3;++i)stream.Append(p[i]);}
struct Patch {float edge[3]:SV_TessFactor;float inside:SV_InsideTessFactor;};
Patch ConstantPatch(InputPatch<P,3> p){Patch o;o.edge[0]=o.edge[1]=o.edge[2]=o.inside=1;return o;}
[domain("tri")][partitioning("integer")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("ConstantPatch")]
P HS(InputPatch<P,3> p,uint id:SV_OutputControlPointID){return p[id];}
[domain("tri")] P DS(Patch data,const OutputPatch<P,3> p,float3 b:SV_DomainLocation){P o;o.p=p[0].p*b.x+p[1].p*b.y+p[2].p*b.z;return o;}
AppendStructuredBuffer<uint> values:register(u5);
[numthreads(1,1,1)]void CS(){values.Append(12345);}
)HLSL";
    auto vsCode = Compile(shader, "VS", "vs_5_0");
    auto psCode = Compile(shader, "PS", "ps_5_0"); auto gsCode = Compile(shader, "GS", "gs_5_0");
    auto hsCode = Compile(shader, "HS", "hs_5_0"); auto dsCode = Compile(shader, "DS", "ds_5_0");
    auto csCode = Compile(shader, "CS", "cs_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11GeometryShader> gs;
    ComPtr<ID3D11HullShader> hs; ComPtr<ID3D11DomainShader> ds; ComPtr<ID3D11ComputeShader> cs;
    HR(test.device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs));
    HR(test.device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps));
    HR(test.device->CreateGeometryShader(gsCode->GetBufferPointer(), gsCode->GetBufferSize(), nullptr, &gs));
    HR(test.device->CreateHullShader(hsCode->GetBufferPointer(), hsCode->GetBufferSize(), nullptr, &hs));
    HR(test.device->CreateDomainShader(dsCode->GetBufferPointer(), dsCode->GetBufferSize(), nullptr, &ds));
    HR(test.device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs));
    ComPtr<ID3D11InputLayout> layout;
    D3D11_INPUT_ELEMENT_DESC element{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    HR(test.device->CreateInputLayout(&element, 1, vsCode->GetBufferPointer(), vsCode->GetBufferSize(), &layout));
    ComPtr<ID3D11Buffer> vertex, index, constants, stream;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 4096; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    HR(test.device->CreateBuffer(&bd, nullptr, &vertex)); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    HR(test.device->CreateBuffer(&bd, nullptr, &index)); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    HR(test.device->CreateBuffer(&bd, nullptr, &constants)); bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
    HR(test.device->CreateBuffer(&bd, nullptr, &stream));
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    HR(test.device->CreateSamplerState(&sd, &sampler));
    ComPtr<ID3D11RasterizerState> raster; D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_WIREFRAME; rd.CullMode = D3D11_CULL_FRONT; rd.ScissorEnable = TRUE;
    HR(test.device->CreateRasterizerState(&rd, &raster));
    ComPtr<ID3D11BlendState> blend; D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
    HR(test.device->CreateBlendState(&blendDesc, &blend));
    ComPtr<ID3D11Texture2D> depthTexture; ComPtr<ID3D11DepthStencilView> depth;
    D3D11_TEXTURE2D_DESC depthDesc{}; depthDesc.Width = depthDesc.Height = 64;
    depthDesc.MipLevels = depthDesc.ArraySize = depthDesc.SampleDesc.Count = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    HR(test.device->CreateTexture2D(&depthDesc, nullptr, &depthTexture));
    HR(test.device->CreateDepthStencilView(depthTexture.Get(), nullptr, &depth));
    test.context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .25f, 63);
    auto depthBefore = Read(test.device.Get(), test.context.Get(), depthTexture.Get());
    ComPtr<ID3D11DepthStencilState> depthState; D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthFunc = D3D11_COMPARISON_NEVER; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    HR(test.device->CreateDepthStencilState(&dd, &depthState));
    std::array<ComPtr<ID3D11Buffer>, 2> outputs;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> uavs;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = bd.ByteWidth / 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    for (int i = 0; i < 2; ++i) { HR(test.device->CreateBuffer(&bd, nullptr, &outputs[i])); HR(test.device->CreateUnorderedAccessView(outputs[i].Get(), &ud, &uavs[i])); }
    ComPtr<ID3D11Predicate> predicate; D3D11_QUERY_DESC query{D3D11_QUERY_OCCLUSION_PREDICATE, 0};
    HR(test.device->CreatePredicate(&query, &predicate));
    test.context->Begin(predicate.Get()); test.context->End(predicate.Get());
    BOOL visible = TRUE; HRESULT ready = S_FALSE;
    for (int i = 0; i < 1000 && ready == S_FALSE; ++i) { ready = test.context->GetData(predicate.Get(), &visible, sizeof(visible), 0); if (ready == S_FALSE) Sleep(1); }
    Check(ready == S_OK && !visible, "producer predicate is resolved false");
    // Commands are suppressed when the result EQUALS PredicateValue.
    test.context->SetPredication(predicate.Get(), FALSE);
    const float unexpected[4]{1, 0, 0, 1};
    test.context->ClearRenderTargetView(target.rtv.Get(), unexpected);
    test.context->SetPredication(nullptr, FALSE);
    auto predicateProbe = Read(test.device.Get(), test.context.Get(), target.texture.Get());
    Check(predicateProbe[0] != 255 && predicateProbe[1] != 0,
        "producer predicate actually suppresses a target-writing operation");
    test.context->SetPredication(predicate.Get(), FALSE);
    test.context->IASetInputLayout(layout.Get()); test.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    UINT stride = 16, offset = 32; test.context->IASetVertexBuffers(0, 1, vertex.GetAddressOf(), &stride, &offset);
    test.context->IASetIndexBuffer(index.Get(), DXGI_FORMAT_R32_UINT, 16);
    offset = 128; test.context->SOSetTargets(1, stream.GetAddressOf(), &offset);
    test.context->VSSetShader(vs.Get(), nullptr, 0); test.context->PSSetShader(ps.Get(), nullptr, 0);
    test.context->GSSetShader(gs.Get(), nullptr, 0); test.context->HSSetShader(hs.Get(), nullptr, 0);
    test.context->DSSetShader(ds.Get(), nullptr, 0); test.context->CSSetShader(cs.Get(), nullptr, 0);
    UINT first = 16, number = 16;
#define BIND(prefix) do { \
    test.context1->prefix##SetConstantBuffers1(0, 1, constants.GetAddressOf(), &first, &number); \
    test.context1->prefix##SetConstantBuffers1(13, 1, constants.GetAddressOf(), &first, &number); \
    test.context->prefix##SetShaderResources(0, 1, target.srv.GetAddressOf()); \
    test.context->prefix##SetShaderResources(127, 1, target.srv.GetAddressOf()); \
    test.context->prefix##SetSamplers(0, 1, sampler.GetAddressOf()); \
    test.context->prefix##SetSamplers(15, 1, sampler.GetAddressOf()); \
} while (false)
    BIND(VS); BIND(PS); BIND(GS); BIND(HS); BIND(DS); BIND(CS);
#undef BIND
    test.context->RSSetState(raster.Get());
    D3D11_VIEWPORT views[2]{{1, 2, 11, 12, .1f, .8f}, {22, 23, 14, 15, .2f, .9f}};
    D3D11_RECT scissors[2]{{3, 4, 8, 9}, {10, 11, 16, 17}};
    test.context->RSSetViewports(2, views); test.context->RSSetScissorRects(2, scissors);
    const float factors[4]{.1f, .2f, .3f, .4f}; test.context->OMSetBlendState(blend.Get(), factors, 0x13579);
    test.context->OMSetDepthStencilState(depthState.Get(), 42);
    UINT count = 7;
    test.context->OMSetRenderTargetsAndUnorderedAccessViews(1, producer.rtv.GetAddressOf(), depth.Get(),
        test.uavSlots - 1, 1, uavs[0].GetAddressOf(), &count);
    count = 9; test.context->CSSetUnorderedAccessViews(5, 1, uavs[1].GetAddressOf(), &count);
    Pipeline before(test.context.Get(), test.uavSlots);
    BlackoutFrame frame; frame.frameId = 9; frame.mask.outer = true;
    D3D11_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
    Check(test.renderer.Apply(test.context.Get(), target.rtv.Get(), frame, 0, viewport), "apply within hostile producer state");
    Pipeline after(test.context.Get(), test.uavSlots); before.Equal(after);
    test.context->SetPredication(nullptr, FALSE);
    // The producer resumes without rebinding; preserve its append position.
    test.context->Dispatch(1, 1, 1);
    ComPtr<ID3D11Buffer> countBuffer, readCount;
    bd = {}; bd.ByteWidth = 4; HR(test.device->CreateBuffer(&bd, nullptr, &countBuffer));
    test.context->CopyStructureCount(countBuffer.Get(), 0, uavs[1].Get());
    bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    HR(test.device->CreateBuffer(&bd, nullptr, &readCount)); test.context->CopyResource(readCount.Get(), countBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{}; HR(test.context->Map(readCount.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    Check(*static_cast<UINT*>(mapped.pData) == 10, "producer dispatch resumes with its UAV append counter intact");
    test.context->Unmap(readCount.Get(), 0);
    auto actual = Read(test.device.Get(), test.context.Get(), target.texture.Get());
    Check(actual[0] == 0 && actual[1] == 0 && actual[2] == 0 && actual[3] == 255, "fill ignores producer predication/blend/depth/scissor/stages");
    Check(actual[(32 * 64 + 32) * 4] != 0, "visible center survives hostile state");
    Check(Read(test.device.Get(), test.context.Get(), producer.texture.Get()) == producerBefore, "producer render target untouched");
    Check(Read(test.device.Get(), test.context.Get(), depthTexture.Get()) == depthBefore, "scene depth and stencil untouched");
    test.context->ClearState();
    Pipeline emptyBefore(test.context.Get(), test.uavSlots);
    Check(test.renderer.Apply(test.context.Get(), target.rtv.Get(), frame, 1, viewport, true), "second eye reuses isolated state");
    Pipeline emptyAfter(test.context.Get(), test.uavSlots); emptyBefore.Equal(emptyAfter);
}

int main()
{
    try {
        unsigned completed = 0;
        for (auto driver : {D3D_DRIVER_TYPE_WARP, D3D_DRIVER_TYPE_HARDWARE})
            for (auto level : {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1}) {
                Fixture test;
                if (!test.Start(driver, level)) {
                    std::printf("SKIP driver=%u level=%x unavailable with debug layer\n", driver, level); continue;
                }
                PixelTests(test); StateTests(test); test.DebugClean(); ++completed;
                std::printf("PASS driver=%u level=%x pixel unions/flips/subrects and complete pipeline restoration\n", driver, level);
            }
        Check(completed >= 2, "at least two feature-level/device configurations exercised");
        std::printf("PASS %u checks\n", checks); return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
