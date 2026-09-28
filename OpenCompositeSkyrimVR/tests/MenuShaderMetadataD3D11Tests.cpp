#include "OpenOVR/Compositor/MenuShaderMetadata.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
static unsigned checks = 0;
static void Check(bool result, const char* message)
{
    ++checks;
    if (!result) throw std::runtime_error(message);
}
static void HR(HRESULT result) { Check(SUCCEEDED(result), "D3D11 operation failed"); }
static ComPtr<ID3DBlob> Compile(const std::string& source)
{
    ComPtr<ID3DBlob> result, errors;
    const auto status = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &result, &errors);
    if (FAILED(status) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer()));
    HR(status);
    return result;
}

int main() try
{
    const std::string texture = "Texture2D<float4> image:register(t0); SamplerState s:register(s0); ";
    const std::string ordinary = texture +
        "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv); }";
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    auto ordinaryBytes = Compile(ordinary);
    ComPtr<ID3D11PixelShader> legacy;
    HR(device->CreatePixelShader(ordinaryBytes->GetBufferPointer(), ordinaryBytes->GetBufferSize(), nullptr, &legacy));
    Check(!ocu_menu::ShaderCanScale(nullptr), "null shader rejected");
    Check(!ocu_menu::ShaderCanScale(legacy.Get()), "pre-hook shader lacks menu permission");
    Check(ocu_vrs_guard::InstallShaderCapture(device.Get()), "production CreatePixelShader hook installed");

    struct Case { const char* name; std::string source; bool allowed; };
    const Case cases[] = {
        {"ordinary normalized UV", ordinary, true},
        {"repeated samples of t0", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+image.Sample(s,uv+0.1); }", true},
        {"unused position parameter", texture +
            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv); }", true},
        {"UV alpha discard stays resolution independent", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { float4 c=image.Sample(s,uv); clip(c.a-0.5); return c; }", true},
        {"raster xy read", texture +
            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+p.x; }", false},
        {"raster z read", texture +
            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+p.z; }", false},
        {"raster w read", texture +
            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+p.w; }", false},
        {"integer texture load", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Load(int3(uv*16,0)); }", false},
        {"load mixed with ordinary sample", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Load(int3(uv*16,0))+image.Sample(s,uv); }", false},
        {"explicit LOD", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.SampleLevel(s,uv,0); }", false},
        {"LOD bias", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.SampleBias(s,uv,1); }", false},
        {"resource dimensions", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { uint w,h; image.GetDimensions(w,h); return image.Sample(s,uv/float2(w,h)); }", false},
        {"LOD query", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+image.CalculateLevelOfDetail(s,uv); }", false},
        {"raster derivative", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+ddx(uv.x); }", false},
        {"second actual texture", texture + "Texture2D<float4> other:register(t1); " +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv)+other.Sample(s,uv); }", false},
        {"sample only t1", "Texture2D<float4> other:register(t1); SamplerState s:register(s0); "
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return other.Sample(s,uv); }", false},
        {"unused texture declaration", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { return float4(uv,0,1); }", false},
        {"second color target", texture +
            "struct Out { float4 a:SV_Target0; float4 b:SV_Target1; }; "
            "Out main(float2 uv:TEXCOORD0) { Out o; o.a=image.Sample(s,uv); o.b=o.a; return o; }", false},
        {"target1 only", texture +
            "float4 main(float2 uv:TEXCOORD0):SV_Target1 { return image.Sample(s,uv); }", false},
        {"depth output", texture +
            "struct Out { float4 a:SV_Target0; float z:SV_Depth; }; "
            "Out main(float2 uv:TEXCOORD0) { Out o; o.a=image.Sample(s,uv); o.z=uv.x; return o; }", false},
        {"UAV output", texture + "RWTexture2D<float4> side:register(u1); "
            "float4 main(float2 uv:TEXCOORD0):SV_Target0 { float4 c=image.Sample(s,uv); side[int2(uv*8)]=c; return c; }", false},
        {"non-Texture2D sampling", "TextureCube<float4> image:register(t0); SamplerState s:register(s0); "
            "float4 main(float3 uv:TEXCOORD0):SV_Target0 { return image.Sample(s,uv); }", false},
    };

    ComPtr<ID3D11PixelShader> scalable;
    for (const auto& test : cases) {
        auto bytes = Compile(test.source);
        for (bool stripped : {false, true}) {
            ComPtr<ID3DBlob> strippedBytes;
            if (stripped) HR(D3DStripShader(bytes->GetBufferPointer(), bytes->GetBufferSize(),
                D3DCOMPILER_STRIP_REFLECTION_DATA | D3DCOMPILER_STRIP_DEBUG_INFO |
                D3DCOMPILER_STRIP_TEST_BLOBS, &strippedBytes));
            auto* input = stripped ? strippedBytes.Get() : bytes.Get();
            const auto expectedReasons = ocu_vrs_guard::ClassifyBytecode(input->GetBufferPointer(), input->GetBufferSize());
            const auto expectedCoarse = ocu_vrs_guard::ClassifyCoarseShadingBytecode(input->GetBufferPointer(), input->GetBufferSize());
            ComPtr<ID3D11PixelShader> shader;
            HR(device->CreatePixelShader(input->GetBufferPointer(), input->GetBufferSize(), nullptr, &shader));
            Check(ocu_menu::ShaderCanScale(shader.Get()) == test.allowed, test.name);
            Check(ocu_vrs_guard::ShaderReasons(shader.Get()) == expectedReasons, "shared VRS/RDM metadata preserved");
            Check(ocu_vrs_guard::ShaderCoarseHazards(shader.Get()) == expectedCoarse, "hardware VRS metadata preserved");
            if (test.allowed) scalable = shader;
        }
    }

    ComPtr<ID3D11ClassLinkage> linkage;
    HR(device->CreateClassLinkage(&linkage));
    ComPtr<ID3D11PixelShader> linked;
    HR(device->CreatePixelShader(ordinaryBytes->GetBufferPointer(), ordinaryBytes->GetBufferSize(), linkage.Get(), &linked));
    Check(!ocu_menu::ShaderCanScale(linked.Get()), "class-linkage creation rejected");

    const auto reasons = ocu_vrs_guard::ShaderReasons(scalable.Get());
    const auto coarse = ocu_vrs_guard::ShaderCoarseHazards(scalable.Get());
    const auto outputs = ocu_vrs_guard::ShaderColorOutputs(scalable.Get());
    const auto material = ocu_vrs_guard::ShaderSingleSampledTexture2D(scalable.Get());
    const ocu_menu::detail::ShaderScaleMetadata future{2, 1};
    HR(scalable->SetPrivateData(ocu_menu::detail::ShaderScaleMetadataId, sizeof(future), &future));
    Check(!ocu_menu::ShaderCanScale(scalable.Get()), "unknown metadata version rejected");
    const unsigned oversized[]{1, 1, 1};
    HR(scalable->SetPrivateData(ocu_menu::detail::ShaderScaleMetadataId, sizeof(oversized), oversized));
    Check(!ocu_menu::ShaderCanScale(scalable.Get()), "wrong metadata size rejected");
    const ocu_menu::detail::ShaderScaleMetadata invalidPermission{1, 2};
    HR(scalable->SetPrivateData(ocu_menu::detail::ShaderScaleMetadataId, sizeof(invalidPermission), &invalidPermission));
    Check(!ocu_menu::ShaderCanScale(scalable.Get()), "unknown permission rejected");
    HR(scalable->SetPrivateData(ocu_menu::detail::ShaderScaleMetadataId, 0, nullptr));
    Check(!ocu_menu::ShaderCanScale(scalable.Get()), "removed metadata rejected");
    Check(ocu_vrs_guard::ShaderReasons(scalable.Get()) == reasons &&
        ocu_vrs_guard::ShaderCoarseHazards(scalable.Get()) == coarse &&
        ocu_vrs_guard::ShaderColorOutputs(scalable.Get()) == outputs &&
        ocu_vrs_guard::ShaderSingleSampledTexture2D(scalable.Get()) == material,
        "menu metadata mutation cannot alter existing VRS/RDM policies");
    std::printf("PASS: %u menu shader checks using compiled/stripped DXBC and production capture\n", checks);
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
