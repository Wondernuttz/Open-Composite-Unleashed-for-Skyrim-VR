// Real NVIDIA GPU regression for production terrain-depth classification.
// The unguarded control bypasses only the new hazard to reproduce old behavior.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/VRSManager.h"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*,long,const char*,const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*,long,const char*,const char* format,...) {
    va_list args; va_start(args,format); std::vprintf(format,args); va_end(args); std::puts("");
}
void Check(bool value,const char* reason) { if(!value) throw std::runtime_error(reason); }
void HR(HRESULT value) { Check(SUCCEEDED(value),"D3D11 call failed"); }
struct Target { ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11RenderTargetView> rtv; ComPtr<ID3D11ShaderResourceView> srv; };
Target Color(ID3D11Device* device,UINT width,UINT height,DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=format; desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    Target target; HR(device->CreateTexture2D(&desc,nullptr,&target.texture));
    HR(device->CreateRenderTargetView(target.texture.Get(),nullptr,&target.rtv));
    HR(device->CreateShaderResourceView(target.texture.Get(),nullptr,&target.srv)); return target;
}
ComPtr<ID3DBlob> Compile(const char* source,const char* target) {
    ComPtr<ID3DBlob> result,error;
    const auto hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",target,0,0,&result,&error);
    if(FAILED(hr) && error) std::printf("%s\n",static_cast<const char*>(error->GetBufferPointer()));
    HR(hr); return result;
}
static VRSManager* manager=nullptr;
static ocu_vrs_scope::SceneScope* sceneScope=nullptr;
static bool useCoarseGuard=true;
static bool applied=false;
void Changed(ID3D11DeviceContext* context,bool targetsChanged) {
    ID3D11RenderTargetView* views[8]{}; ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(8,views,&depth);
    const auto* viewports=ocu_vrs_guard::CurrentViewports(context);
    const bool scene=sceneScope->Matches(context,8,views,depth.Get());
    const bool eligible=scene && viewports && manager->UpdateActiveViewports(viewports->count,viewports->values)
        && ocu_vrs_guard::CurrentReasons(context)==ocu_vrs_guard::Compatible
        && (!useCoarseGuard || ocu_vrs_guard::CurrentCoarseHazards(context)==ocu_vrs_guard::CoarseCompatible);
    for(auto* view:views) if(view) view->Release();
    if(targetsChanged && applied) { manager->Disable(); applied=false; }
    if(eligible && !applied) applied=manager->ApplyStereo();
    if(!eligible && applied) { manager->Disable(); applied=false; }
}
int main() try {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> next; if(factory->EnumAdapters1(i,&next)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{}; HR(next->GetDesc1(&desc));
        if(desc.VendorId==0x10de) {adapter=next; std::printf("GPU: %ls\n",desc.Description);break;}
    }
    Check(adapter!=nullptr,"NVIDIA GPU required");
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    Check(ocu_vrs_guard::InstallShaderCapture(device.Get()),"production shader capture installed");
    constexpr UINT width=256,height=128;
    auto mask=Color(device.Get(),width,height,DXGI_FORMAT_R32_FLOAT);
    auto diffuse=Color(device.Get(),width,height,DXGI_FORMAT_R32G32B32A32_FLOAT);
    auto albedo=Color(device.Get(),width,height,DXGI_FORMAT_R32G32B32A32_FLOAT);
    D3D11_TEXTURE2D_DESC dd{}; dd.Width=width;dd.Height=height;dd.MipLevels=1;dd.ArraySize=1;
    dd.Format=DXGI_FORMAT_D32_FLOAT;dd.SampleDesc.Count=1;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth; ComPtr<ID3D11DepthStencilView> dsv;
    HR(device->CreateTexture2D(&dd,nullptr,&depth));HR(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
    const char* vsCode="float4 main(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0.2+uv.x*0.4+uv.y*0.2,1);}";
    const char* maskCode="float main(float4 p:SV_Position):SV_Target {return p.z;}";
    // Same-surface equality and alpha formula from CSX Lighting terrain blending.
    const char* terrainCode=R"(
Texture2D<float> depthMask:register(t0);
float Linear(float z) {return 1.0/(1.0-z);}
struct Out {float4 diffuse:SV_Target0;float4 albedo:SV_Target1;};
Out main(float4 p:SV_Position) {
    float sampled=depthMask[uint2(p.xy)];
    float blend=saturate((Linear(sampled)-Linear(p.z))/10.0);
    if(p.z==sampled) blend=1;
    Out result;result.diffuse=float4(0,1,0,blend);result.albedo=float4(1,0,0,blend);return result;
})";
    const char* opaqueCode="float4 main(float4 p:SV_Position):SV_Target {return float4(p.xy,0.5,1);}";
    auto vsBytes=Compile(vsCode,"vs_5_0"),maskBytes=Compile(maskCode,"ps_5_0"),terrainBytes=Compile(terrainCode,"ps_5_0"),opaqueBytes=Compile(opaqueCode,"ps_5_0");
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> maskPS,terrainPS,opaquePS;
    HR(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,&vs));
    HR(device->CreatePixelShader(maskBytes->GetBufferPointer(),maskBytes->GetBufferSize(),nullptr,&maskPS));
    ComPtr<ID3DBlob> strippedTerrain;
    HR(D3DStripShader(terrainBytes->GetBufferPointer(),terrainBytes->GetBufferSize(),
        D3DCOMPILER_STRIP_REFLECTION_DATA|D3DCOMPILER_STRIP_DEBUG_INFO,&strippedTerrain));
    HR(device->CreatePixelShader(strippedTerrain->GetBufferPointer(),strippedTerrain->GetBufferSize(),nullptr,&terrainPS));
    HR(device->CreatePixelShader(opaqueBytes->GetBufferPointer(),opaqueBytes->GetBufferSize(),nullptr,&opaquePS));
    std::printf("PRODUCTION_GUARD terrainShared=0x%X terrainCoarse=0x%X opaqueCoarse=0x%X\n",ocu_vrs_guard::ShaderReasons(terrainPS.Get()),ocu_vrs_guard::ShaderCoarseHazards(terrainPS.Get()),ocu_vrs_guard::ShaderCoarseHazards(opaquePS.Get()));
    Check(ocu_vrs_guard::ShaderReasons(terrainPS.Get())==ocu_vrs_guard::Compatible,"shared RDM terrain classification remains unchanged");
    Check(ocu_vrs_guard::ShaderCoarseHazards(terrainPS.Get())==ocu_vrs_guard::RasterDepthTextureLoad,"production guard protects stripped terrain depth-load shader");
    Check(ocu_vrs_guard::ShaderCoarseHazards(opaquePS.Get())==ocu_vrs_guard::CoarseCompatible,"production guard retains opaque VRS");
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rd,&raster));context->RSSetState(raster.Get());
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> writeDepth,equalDepth;
    HR(device->CreateDepthStencilState(&ds,&writeDepth));ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_EQUAL;
    HR(device->CreateDepthStencilState(&ds,&equalDepth));
    D3D11_BLEND_DESC bd{};auto& blend=bd.RenderTarget[0];blend.BlendEnable=TRUE;
    blend.SrcBlend=D3D11_BLEND_SRC_ALPHA;blend.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D11_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D11_BLEND_ONE;blend.DestBlendAlpha=D3D11_BLEND_ZERO;blend.BlendOpAlpha=D3D11_BLEND_OP_ADD;blend.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> alphaBlend;HR(device->CreateBlendState(&bd,&alphaBlend));
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);
    auto draw=[&]() {
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;HR(device->CreateQuery(&qd,&query));context->Begin(query.Get());
        for(int eye=0;eye<2;++eye) {D3D11_VIEWPORT viewport{float(eye*width/2),0,float(width/2),float(height),0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);}
        context->End(query.Get());D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};HRESULT result=S_FALSE;
        for(int i=0;i<1000 && result==S_FALSE;++i) {result=context->GetData(query.Get(),&stats,sizeof(stats),0);if(result==S_FALSE)Sleep(1);}
        Check(result==S_OK,"GPU stats completed");return stats.PSInvocations;
    };
    auto wrongPixels=[&](const Target& target,int component) {
        D3D11_TEXTURE2D_DESC desc{};target.texture->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&desc,nullptr,&staging));context->CopyResource(staging.Get(),target.texture.Get());
        D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
        std::array<UINT,5> result{};
        for(UINT y=0;y<height;++y){const float* row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
            for(UINT x=0;x<width;++x){if(row[4*x+component]<0.99f){++result[0];++result[3+x/(width/2)];}if(row[4*x+3]<0.01f)++result[1];if(row[4*x+3]>0.99f)++result[2];}}
        context->Unmap(staging.Get(),0);return result;
    };
    VRSManager vrs;Check(vrs.Initialize(device.Get()),"production NVAPI backend initialized");manager=&vrs;
    ocu_vrs_scope::SceneScope scope;scope.Arm(context.Get(),depth.Get(),diffuse.texture.Get(),width,height);sceneScope=&scope;
    auto run=[&](ocu_foveation::Rate rate,bool protect) {
        ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;useCoarseGuard=protect;
        ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
        auto* maskRTV=mask.rtv.Get();context->OMSetRenderTargets(1,&maskRTV,dsv.Get());context->OMSetBlendState(nullptr,nullptr,~0u);context->OMSetDepthStencilState(writeDepth.Get(),0);context->PSSetShader(maskPS.Get(),nullptr,0);draw();
        const float blue[4]={0.2f,0.35f,0.8f,1},brown[4]={0.8f,0.5f,0.2f,1};context->ClearRenderTargetView(diffuse.rtv.Get(),blue);context->ClearRenderTargetView(albedo.rtv.Get(),brown);
        ID3D11RenderTargetView* colors[]={diffuse.rtv.Get(),albedo.rtv.Get()};context->OMSetRenderTargets(2,colors,dsv.Get());
        auto* srv=mask.srv.Get();context->PSSetShaderResources(0,1,&srv);context->OMSetDepthStencilState(equalDepth.Get(),0);context->OMSetBlendState(alphaBlend.Get(),nullptr,~0u);context->PSSetShader(terrainPS.Get(),nullptr,0);
        Check(vrs.UpdateStereoPattern(width,height,{0,0,width/2,height},{width/2,0,width/2,height},0.3f,0.6f,{ocu_foveation::Rate::X1x1,rate,rate}),"production pattern ready");
        Check(ocu_vrs_guard::WatchContext(context.Get(),&Changed),"production state hooks active");Changed(context.Get(),true);
        const auto invocations=draw();auto d=wrongPixels(diffuse,1),a=wrongPixels(albedo,0);
        std::printf("TERRAIN rate=%s productionGuard=%d PS=%llu diffuseWrong=%u albedoWrong=%u wrongLeft=%u wrongRight=%u nearZeroAlpha=%u opaquePixels=%u\n",ocu_foveation::RateName(rate),int(protect),invocations,d[0],a[0],d[3],d[4],d[1],d[2]);
        if (protect || rate==ocu_foveation::Rate::X1x1) {
            Check(d[0]==0 && a[0]==0 && d[1]==0 && a[1]==0,"full-rate protection restores both MRTs and both eyes");
            Check(invocations==width*height,"terrain shader runs at full rate");
        } else {
            Check(d[3]>0 && d[4]>0 && a[3]>0 && a[4]>0,"unguarded coarse control loses color in both eyes and MRTs");
        }
        if (protect && rate!=ocu_foveation::Rate::X1x1) {
            // No unwatch/rearm between terrain and the following opaque draw:
            // the production PSSetShader hook must restore coarse shading.
            context->PSSetShader(opaquePS.Get(),nullptr,0);
            const auto nextOpaque=draw();
            Check(nextOpaque<width*height*0.8,"next opaque shader regains coarse shading without rearm");
            std::printf("OPAQUE_TRANSITION rate=%s PS=%llu\n",ocu_foveation::RateName(rate),nextOpaque);
        }
        ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;return d;
    };
    auto full=run(ocu_foveation::Rate::X1x1,false);Check(full[0]==0,"full-rate terrain exactly covers same-surface mask");
    for(auto rate:{ocu_foveation::Rate::X2x1,ocu_foveation::Rate::X1x2,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X4x4}) {
        const auto coarse=run(rate,false),protectedResult=run(rate,true);
        Check(coarse[0]>0,"unguarded control reproduces slope terrain color loss under coarse shading");Check(protectedResult[0]==0,"production guard restores terrain");
    }
    context->OMSetBlendState(nullptr,nullptr,~0u);context->OMSetDepthStencilState(writeDepth.Get(),0);auto* rtv=diffuse.rtv.Get();context->OMSetRenderTargets(1,&rtv,dsv.Get());context->PSSetShader(opaquePS.Get(),nullptr,0);
    vrs.Disable();const auto opaqueFull=draw();useCoarseGuard=true;
    Check(ocu_vrs_guard::WatchContext(context.Get(),&Changed),"opaque control watches same production guard");Changed(context.Get(),true);const auto opaqueCoarse=draw();
    std::printf("OPAQUE PS full=%llu coarse=%llu saved=%.2f%%\n",opaqueFull,opaqueCoarse,100.0*(1.0-double(opaqueCoarse)/opaqueFull));
    Check(opaqueCoarse<opaqueFull*0.8,"selective terrain protection retains opaque VRS reduction");
    ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();
    std::puts("PASS: real GPU terrain depth-load mismatch reproduced; production guard restores both MRTs and eyes, opaque savings retained");return 0;
} catch(const std::exception& error) {std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
