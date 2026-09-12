#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/ExactPixelShader.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string_view>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace ocu_vrs_guard {
std::uint32_t ClassifyCoarseShadingAssemblyForTest(std::string_view) noexcept;
}
using namespace ocu_vrs_guard;
static unsigned checks = 0;
static void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr), "D3D call failed"); }
static ComPtr<ID3DBlob> Compile(const char* source) {
    ComPtr<ID3DBlob> blob, error;
    auto hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "ps_5_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error);
    if (FAILED(hr) && error) std::puts(static_cast<const char*>(error->GetBufferPointer()));
    HR(hr); return blob;
}
static unsigned notifications = 0;
static void Changed(ID3D11DeviceContext*, bool) { ++notifications; }
int main(int argc, char** argv) try {
    const std::string position = "ps_5_0\ndcl_input_ps_siv linear noperspective v1.xyzw, position\n";
    const std::string texture = "dcl_resource_texture2d (float,float,float,float) t1\n";
    const std::string load = "ld_indexable(texture2d)(float,float,float,float) r0, r1.xyzw, t1.xyzw\n";
    auto assembly = [&](std::string text, bool hazardous, const char* why) {
        Check(ClassifyCoarseShadingAssemblyForTest(text) == (hazardous ? RasterDepthTextureLoad : CoarseCompatible), why);
    };
    for (const char* operand : {"v1.z", "v1.zzzz", "v1.xyzz", "v1", "-v1.z", "abs(v1.zzzz)", "-abs(v1.z)"})
        assembly(position + texture + load + "mov r0, " + operand + "\n", true, "position z swizzle/unmasked/modifier read");
    for (const char* operand : {"v1.xyxx", "v1.xxxx", "v10.z", "v11.zzzz", "v0.z"})
        assembly(position + texture + load + "mov r0, " + operand + "\n", false, "register boundary and non-z read");
    assembly(position + texture + load, false, "declaration alone does not imply executable raster-depth read");
    assembly(position + texture + load + "// mov r0, v1.z\n", false, "comments do not imply reads");
    assembly(position + texture + "mov r0, v1.z\n", false, "texture declaration alone does not imply load");
    assembly(position + texture + "ld r0, r1, t1\nmov r0, v1.z\n", true, "plain ld uses Texture2D declaration");
    assembly(position + texture + "ld r0, r1, t10\nmov r0, v1.z\n", false, "resource register boundaries");
    assembly(position + "dcl_resource_buffer (float,float,float,float) t1\nld r0, r1, t1\nmov r0, v1.z\n", false, "buffer load excluded");
    assembly(position + "dcl_resource_texture3d (float,float,float,float) t1\nld_indexable(texture3d)(float,float,float,float) r0, r1, t1\nmov r0, v1.z\n", false, "3D load excluded");
    assembly(position + "dcl_resource_texture2darray (float,float,float,float) t1\n" + load + "mov r0, v1.z\n", false, "array declaration is not Texture2D");
    assembly("ps_5_0\ndcl_input_ps linear v1.xyz\n" + texture + load + "mov r0, v1.z\n", false, "ordinary varying z is not raster depth");
    assembly("ps_5_0\ndcl_input_ps_siv linear noperspective v10.xyz, position\n" + texture + load + "mov r0, v10.z\n", true, "two-digit position register");
    Check(ClassifyCoarseShadingBytecode(nullptr, 0) == CoarseUnclassified, "invalid bytecode fails safe");
    const char* terrain = "Texture2D<float> d:register(t0); float4 main(float4 p:SV_Position):SV_Target {float z=d[uint2(p.xy)]; return float4(z,p.z==z,abs(p.z-z),1);}";
    struct Case { const char* name; const char* source; bool hazard; int sampledSlot = -1; };
    const Case cases[] = {
        {"terrain", terrain, true},
        {"position_xy_load", "Texture2D<float> d:register(t0); float4 main(float4 p:SV_Position):SV_Target {return d[uint2(p.xy)];}", false},
        {"position_z_no_load", "float4 main(float4 p:SV_Position):SV_Target {return p.z;}", false},
        {"sampled_texture", "Texture2D<float> d:register(t0); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return d.SampleLevel(s,p.xy,0)+p.z;}", false, 0},
        {"repeated_sample", "Texture2D<float> d:register(t10); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return d.Sample(s,p.xy)+d.Sample(s,p.xy+1);}", false, 10},
        {"last_texture_slot", "Texture2D<float> d:register(t127); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return d.Sample(s,p.xy);}", false, 127},
        {"multiple_material_slots", "Texture2D<float> a:register(t1),b:register(t10); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return a.Sample(s,p.xy)+b.Sample(s,p.xy);}", false},
        {"unused_material_declaration", "Texture2D<float> a:register(t1),b:register(t10); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return b.Sample(s,p.xy);}", false, 10},
        {"cube_sample", "TextureCube<float> a:register(t1); SamplerState s:register(s0); float4 main(float4 p:SV_Position):SV_Target {return a.Sample(s,p.xyz);}", false},
        {"buffer_load", "Buffer<float> d:register(t0); float4 main(float4 p:SV_Position):SV_Target {return d[uint(p.x)]+p.z;}", false},
        {"3d_load", "Texture3D<float> d:register(t0); float4 main(float4 p:SV_Position):SV_Target {return d.Load(int4(p.xyz,0))+p.z;}", false},
        {"ordinary_varying_depth", "Texture2D<float> d:register(t0); float4 main(float4 p:SV_Position,float3 q:TEXCOORD):SV_Target {return d[uint2(p.xy)]+q.z;}", false},
        {"opaque", "float4 main(float4 p:SV_Position):SV_Target {return float4(p.xy,0,1);}", false},
    };
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    auto terrainBytes = Compile(terrain);
    ComPtr<ID3D11PixelShader> oldShader;
    HR(device->CreatePixelShader(terrainBytes->GetBufferPointer(),terrainBytes->GetBufferSize(),nullptr,&oldShader));
    Check(ShaderCoarseHazards(oldShader.Get()) == CoarseUnclassified, "pre-capture shader fails safe for hardware VRS");
    Check(InstallShaderCapture(device.Get()), "capture installed");
    const auto countBefore = Counts().rasterDepthTextureLoad;
    ComPtr<ID3D11PixelShader> terrainShader, opaqueShader, sampledShader;
    for (const auto& test : cases) {
        auto bytes = Compile(test.source);
        for (bool stripped : {false,true}) {
            ComPtr<ID3DBlob> strippedBytes;
            if (stripped) HR(D3DStripShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),
                D3DCOMPILER_STRIP_REFLECTION_DATA|D3DCOMPILER_STRIP_DEBUG_INFO|D3DCOMPILER_STRIP_TEST_BLOBS,&strippedBytes));
            ID3DBlob* input = stripped ? strippedBytes.Get() : bytes.Get();
            const auto expected = test.hazard ? RasterDepthTextureLoad : CoarseCompatible;
            Check(ClassifyCoarseShadingBytecode(input->GetBufferPointer(),input->GetBufferSize()) == expected,test.name);
            Check(ClassifyBytecode(input->GetBufferPointer(),input->GetBufferSize()) == Compatible,"shared RDM classification unchanged");
            ComPtr<ID3D11PixelShader> shader;
            HR(device->CreatePixelShader(input->GetBufferPointer(),input->GetBufferSize(),nullptr,&shader));
            Check(ShaderCoarseHazards(shader.Get()) == expected, "capture metadata stores coarse decision");
            Check(ShaderReasons(shader.Get()) == Compatible, "capture shared metadata unchanged");
            Check(ShaderSingleSampledTexture2D(shader.Get()) == test.sampledSlot, "actual sampled Texture2D slot metadata");
            const auto slots = ShaderSampledTexture2DSlots(shader.Get());
            bool slotsMatch = true;
            for (unsigned slot=0;slot<128;++slot) {
                const bool expectedSlot = int(slot)==test.sampledSlot ||
                    (std::strcmp(test.name,"multiple_material_slots")==0 && (slot==1 || slot==10));
                if (slots.Contains(slot)!=expectedSlot) slotsMatch = false;
            }
            Check(slotsMatch && !slots.Contains(128),"sampled slot set retains all and only actual 2D samples");
            if (std::strcmp(test.name,"terrain") == 0) terrainShader = shader;
            if (std::strcmp(test.name,"opaque") == 0) opaqueShader = shader;
            if (std::strcmp(test.name,"sampled_texture") == 0) sampledShader = shader;
        }
    }
    Check(Counts().rasterDepthTextureLoad == countBefore + 2, "counter measures captured terrain shaders");
    constexpr GUID sharedId{0x8ee350a6,0xb547,0x4869,{0xa3,0xc1,0x65,0x5a,0x0f,0x95,0x62,0x41}};
    constexpr GUID coarseId{0xbc076f82,0x91d0,0x464d,{0x9b,0xce,0x97,0x11,0xa8,0xe9,0x03,0x71}};
    const unsigned v2[]{2,Compatible,1}; HR(oldShader->SetPrivateData(sharedId,sizeof(v2),v2));
    Check(ShaderReasons(oldShader.Get()) == Compatible && ShaderColorOutputs(oldShader.Get()) == 1,"existing v2 metadata preserved for RDM");
    Check(ShaderCoarseHazards(oldShader.Get()) == CoarseUnclassified,"v2 without coarse metadata cannot imply coarse safety");
    Check(ShaderSingleSampledTexture2D(oldShader.Get()) == -1,"old metadata cannot imply material identity");
    for (const auto version : {0u,1u,99u}) {
        const unsigned metadata[]{version,CoarseCompatible,1,0,0,0}; HR(oldShader->SetPrivateData(coarseId,sizeof(metadata),metadata));
        Check(ShaderCoarseHazards(oldShader.Get()) == CoarseUnclassified,"unknown coarse metadata version fails safe");
        Check(ShaderReasons(oldShader.Get()) == Compatible,"coarse metadata never changes RDM result");
        Check(ShaderSingleSampledTexture2D(oldShader.Get()) == -1,"unknown metadata version does not invent material slots");
    }
    const unsigned malformed[]{2,CoarseCompatible}; HR(oldShader->SetPrivateData(coarseId,sizeof(malformed),malformed));
    Check(ShaderCoarseHazards(oldShader.Get()) == CoarseUnclassified,"short coarse metadata fails safe");
    Check(ShaderSingleSampledTexture2D(oldShader.Get()) == -1,"partial metadata cannot provide material identity");
    const unsigned validEmpty[]{2,CoarseCompatible,0,0,0,0}; HR(oldShader->SetPrivateData(coarseId,sizeof(validEmpty),validEmpty));
    Check(ShaderCoarseHazards(oldShader.Get()) == CoarseCompatible && ShaderSingleSampledTexture2D(oldShader.Get()) == -1,
        "valid zero-sample metadata is distinct from missing metadata");
    const unsigned tooLong[]{2,CoarseCompatible,0,0,0,0,0}; HR(oldShader->SetPrivateData(coarseId,sizeof(tooLong),tooLong));
    Check(ShaderCoarseHazards(oldShader.Get()) == CoarseUnclassified,"oversized metadata fails safe");
    context->PSSetShader(terrainShader.Get(),nullptr,0);
    Check(WatchContext(context.Get(),Changed),"context capture installed");
    Check(CurrentReasons(context.Get()) == Compatible && CurrentCoarseHazards(context.Get()) == RasterDepthTextureLoad,"initial refresh separates RDM and VRS policy");
    context->PSSetShader(opaqueShader.Get(),nullptr,0);
    Check(CurrentCoarseHazards(context.Get()) == CoarseCompatible,"next opaque shader restores coarse eligibility");
    context->PSSetShader(terrainShader.Get(),nullptr,0);
    Check(CurrentCoarseHazards(context.Get()) == RasterDepthTextureLoad,"shader bind recaptures terrain hazard");
    context->PSSetShader(sampledShader.Get(),nullptr,0);
    Check(CurrentSingleSampledTexture2D(context.Get()) == 0,"shader bind captures material slot");
    Check(CurrentSampledTexture2DSlots(context.Get()).Contains(0),"shader bind caches sampled slot set");
    context->ClearState();
    Check(CurrentReasons(context.Get()) == NoPixelShader && CurrentCoarseHazards(context.Get()) == CoarseUnclassified,"clear removes stale shader state");
    Check(CurrentSingleSampledTexture2D(context.Get()) == -1,"clear removes stale material slot");
    Check(!CurrentSampledTexture2DSlots(context.Get()).Contains(0),"clear removes stale sampled slot set");
    Check(notifications >= 3,"production hooks notified transitions");
    UnwatchContext(context.Get());
    Check(CurrentCoarseHazards(context.Get()) == CoarseUnclassified,"unwatched contexts fail safe");
    Check(!ocu_exact_pixels::Mark(nullptr)&&!ocu_exact_pixels::IsMarked(nullptr),"exact-pixel tag handles null shader");
    const auto originalOutputs=ShaderColorOutputs(opaqueShader.Get());
    Check(ocu_exact_pixels::Mark(opaqueShader.Get()),"owned shader tag stored after bytecode capture");
    Check(ShaderReasons(opaqueShader.Get())==ExactPixels,"owned exact-pixel tag protects shared VRS/RDM policy");
    Check(ShaderCoarseHazards(opaqueShader.Get())==CoarseCompatible&&ShaderColorOutputs(opaqueShader.Get())==originalOutputs,
        "tag leaves captured coarse metadata and stripped output metadata unchanged");
    unsigned originalMetadata[3]{};UINT originalSize=sizeof(originalMetadata);
    HR(opaqueShader->GetPrivateData(sharedId,&originalSize,originalMetadata));
    Check(originalSize==sizeof(originalMetadata)&&originalMetadata[0]==2&&originalMetadata[1]==Compatible,
        "exact tag preserves original shared metadata ABI and contents");
    const unsigned futureExact[]{99,123};HR(opaqueShader->SetPrivateData(ocu_exact_pixels::MetadataId,sizeof(futureExact),futureExact));
    Check(ShaderReasons(opaqueShader.Get())==ExactPixels,"unfamiliar exact-pixel request remains protected");
    const unsigned char shortExact=1;HR(opaqueShader->SetPrivateData(ocu_exact_pixels::MetadataId,sizeof(shortExact),&shortExact));
    Check(ShaderReasons(opaqueShader.Get())==ExactPixels,"short exact-pixel request remains protected");
    HR(opaqueShader->SetPrivateData(ocu_exact_pixels::MetadataId,0,nullptr));
    Check(ShaderReasons(opaqueShader.Get())==Compatible,"untagged ordinary shaders retain normal eligibility");
    Check(ocu_exact_pixels::Mark(oldShader.Get()),"legacy shader accepts separate exact tag");
    Check(ShaderReasons(oldShader.Get())==ExactPixels&&ShaderCoarseHazards(oldShader.Get())==CoarseUnclassified,
        "tag preserves existing shared classification and missing coarse-metadata fallback");
    for (int i=1;i<argc;++i) {
        std::ifstream stream(argv[i],std::ios::binary);
        Check(bool(stream),"actual CSX bytecode readable");
        std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),{});
        Check(ClassifyCoarseShadingBytecode(bytes.data(),bytes.size()) == RasterDepthTextureLoad,"actual CSX terrain shader protected");
        std::printf("Actual shader protected: %s\n",argv[i]);
    }
    std::printf("PASS: %u terrain guard checks (assembly, stripped DXBC, metadata, RDM separation, context transitions)\n",checks);
    return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
