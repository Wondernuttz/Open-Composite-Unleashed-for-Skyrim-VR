// Real NVIDIA GPU audit of production DAPA mask replay under production VRS.
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
#include <MinHook.h>
#include "DrvOpenXR/DapaPlayerMaskGpu.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/VRSManager.h"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*,long,const char*,const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*,long,const char*,const char* format,...) {
    va_list args;va_start(args,format);std::vprintf(format,args);va_end(args);std::puts("");
}
void Check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
void HR(HRESULT value) {Check(SUCCEEDED(value),"D3D11 call failed");}
ComPtr<ID3DBlob> Compile(const char* code,const char* target) {
    ComPtr<ID3DBlob> blob,error;
    const auto hr=D3DCompile(code,std::strlen(code),nullptr,nullptr,nullptr,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
    if(FAILED(hr)&&error)std::puts(static_cast<const char*>(error->GetBufferPointer()));HR(hr);return blob;
}
static VRSManager* manager;
static ocu_vrs_scope::SceneScope* scope;
static bool applied=false,ignoreExactPixels=false;
void Changed(ID3D11DeviceContext* context,bool targetsChanged) {
    ID3D11RenderTargetView* views[8]{};ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(8,views,&depth);
    const auto* viewports=ocu_vrs_guard::CurrentViewports(context);
    const auto reasons=ocu_vrs_guard::CurrentReasons(context) &
        ~(ignoreExactPixels ? ocu_vrs_guard::ExactPixels : 0u);
    const bool eligible=scope->Matches(context,8,views,depth.Get()) && viewports &&
        manager->UpdateActiveViewports(viewports->count,viewports->values) &&
        reasons==ocu_vrs_guard::Compatible &&
        ocu_vrs_guard::CurrentCoarseHazards(context)==ocu_vrs_guard::CoarseCompatible;
    for(auto* view:views)if(view)view->Release();
    if(targetsChanged&&applied){manager->Disable();applied=false;}
    if(eligible&&!applied)applied=manager->ApplyStereo();
    if(!eligible&&applied){manager->Disable();applied=false;}
}
using SetPrivateDataFn=HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceChild*,REFGUID,UINT,const void*);
static SetPrivateDataFn originalSetPrivateData;
static unsigned tagNumber=0,refuseTagNumber=0;
static HRESULT STDMETHODCALLTYPE RefuseExactTag(ID3D11DeviceChild* child,REFGUID guid,UINT size,const void* value) {
    if(IsEqualGUID(guid,ocu_exact_pixels::MetadataId) && ++tagNumber==refuseTagNumber)return E_OUTOFMEMORY;
    return originalSetPrivateData(child,guid,size,value);
}
void Run(bool d24) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> next;if(factory->EnumAdapters1(i,&next)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};HR(next->GetDesc1(&desc));if(desc.VendorId==0x10de){adapter=next;std::printf("GPU: %ls\n",desc.Description);break;}}
    Check(adapter!=nullptr,"NVIDIA GPU required");
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    Check(ocu_vrs_guard::InstallShaderCapture(device.Get()),"production shader capture installed");
    constexpr UINT width=256,height=128;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=d24?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth,color;ComPtr<ID3D11DepthStencilView> dsv;ComPtr<ID3D11RenderTargetView> rtv;
    HR(device->CreateTexture2D(&desc,nullptr,&depth));
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.Format=d24?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_D32_FLOAT;vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    HR(device->CreateDepthStencilView(depth.Get(),&vd,&dsv));
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    HR(device->CreateTexture2D(&desc,nullptr,&color));HR(device->CreateRenderTargetView(color.Get(),nullptr,&rtv));
    auto vsBytes=Compile("float4 main(uint id:SV_VertexID):SV_Position {float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),0.2+uv.x*0.4+uv.y*0.2,1);}","vs_5_0");
    auto psBytes=Compile("float4 main(float4 p:SV_Position):SV_Target{return float4(p.xy,.5,1);}","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
    HR(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,&vs));
    HR(device->CreatePixelShader(psBytes->GetBufferPointer(),psBytes->GetBufferSize(),nullptr,&ps));
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    ComPtr<ID3D11DepthStencilState> writeDepth;HR(device->CreateDepthStencilState(&ds,&writeDepth));
    D3D11_RASTERIZER_DESC rs{};rs.FillMode=D3D11_FILL_SOLID;rs.CullMode=D3D11_CULL_NONE;rs.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rs,&raster));context->RSSetState(raster.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);
    context->PSSetShader(ps.Get(),nullptr,0);context->OMSetDepthStencilState(writeDepth.Get(),0);
    auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,dsv.Get());
    auto draw=[&]() {
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;HR(device->CreateQuery(&qd,&query));context->Begin(query.Get());
        for(UINT eye=0;eye<2;++eye){D3D11_VIEWPORT vp{float(eye*width/2),0,float(width/2),float(height),0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);}
        context->End(query.Get());D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};HRESULT hr=S_FALSE;
        for(int i=0;i<1000&&hr==S_FALSE;++i){hr=context->GetData(query.Get(),&stats,sizeof(stats),0);if(hr==S_FALSE)Sleep(1);}
        Check(hr==S_OK,"GPU statistics ready");return stats.PSInvocations;
    };
    auto read=[&](ID3D11Texture2D* input) {
        D3D11_TEXTURE2D_DESC copy{};input->GetDesc(&copy);copy.BindFlags=0;copy.Usage=D3D11_USAGE_STAGING;copy.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&copy,nullptr,&staging));context->CopyResource(staging.Get(),input);
        const UINT components=copy.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?4:1;
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<float> data(width*height*components);
        for(UINT y=0;y<height;++y) {
            const auto* row=static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch;
            if(copy.Format==DXGI_FORMAT_R24G8_TYPELESS) {
                const auto* raw=reinterpret_cast<const UINT*>(row);
                for(UINT x=0;x<width;++x)data[y*width+x]=float(raw[x]&0xffffffu)/16777215.0f;
            } else std::memcpy(data.data()+y*width*components,row,width*components*sizeof(float));
        }
        context->Unmap(staging.Get(),0);return data;
    };
    draw();const auto actualDepth=read(depth.Get());
    // Exercise actual production initialization failure at either exact shader
    // tag write. Shader-capture metadata writes still succeed independently.
    auto* tagTarget=(*reinterpret_cast<void***>(ps.Get()))[5];
    Check(MH_CreateHook(tagTarget,reinterpret_cast<void*>(&RefuseExactTag),reinterpret_cast<void**>(&originalSetPrivateData))==MH_OK,"tag failure detour created");
    Check(MH_EnableHook(tagTarget)==MH_OK,"tag failure detour enabled");
    for(unsigned refused:{1u,2u}) {refuseTagNumber=refused;tagNumber=0;DapaPlayerMaskGpu unavailable;
        Check(!unavailable.Initialize(device.Get())&&tagNumber==refused,"failed exact tag refuses unprotected production mask initialization");}
    Check(MH_DisableHook(tagTarget)==MH_OK&&MH_RemoveHook(tagTarget)==MH_OK,"tag failure detour removed");
    DapaPlayerMaskGpu mask;Check(mask.Initialize(device.Get())&&mask.Size(device.Get(),width,height),"production DAPA mask initialized");
    VRSManager vrs;Check(vrs.Initialize(device.Get()),"NVAPI initialized");manager=&vrs;
    ocu_vrs_scope::SceneScope scene;scene.Arm(context.Get(),depth.Get(),color.Get(),width,height);scope=&scene;
    Check(ocu_vrs_guard::WatchContext(context.Get(),Changed),"production state watcher installed");
    for(auto rate:{ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x1,ocu_foveation::Rate::X1x2,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X4x4}) {
        Check(vrs.UpdateStereoPattern(width,height,{0,0,width/2,height},{width/2,0,width/2,height},.3f,.6f,
            {ocu_foveation::Rate::X1x1,rate,rate}),"stereo pattern ready");
        for(bool protectedMask:{false,true}) {
            ignoreExactPixels=!protectedMask;Changed(context.Get(),true);mask.Clear(context.Get());
            const auto beforeReplayColor=read(color.Get());
            unsigned shared=999,coarse=999;unsigned long long invocations=0;
            Check(mask.Replay(context.Get(),[&] {shared=ocu_vrs_guard::CurrentReasons(context.Get());coarse=ocu_vrs_guard::CurrentCoarseHazards(context.Get());invocations=draw();}),"production DAPA replay succeeds");
            auto values=read(mask.Texture());std::array<UINT,2> rejected{};
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto i=y*width+x;
                if(values[i]<0 || values[i]>1 || std::abs(values[i]-actualDepth[i])>0.000002f)++rejected[x/(width/2)];}
            std::printf("MASK format=%s rate=%s productionTag=%d shared=%u coarse=%u PS=%llu rejectedL=%u rejectedR=%u total=%u\n",
                d24?"D24S8":"D32",ocu_foveation::RateName(rate),int(protectedMask),shared,coarse,invocations,rejected[0],rejected[1],rejected[0]+rejected[1]);
            Check(shared==ocu_vrs_guard::ExactPixels,"only explicit owned-shader reason protects mask");
            Check(read(depth.Get())==actualDepth&&read(color.Get())==beforeReplayColor,"production mask replay preserves complete scene color and depth");
            if(protectedMask||rate==ocu_foveation::Rate::X1x1)Check(!rejected[0]&&!rejected[1],"production tag preserves exact mask depth in both eyes");
            else Check(rejected[0]>0&&rejected[1]>0,"current VRS policy reproduces rejected DAPA mask depth in both eyes");
            if(protectedMask&&rate!=ocu_foveation::Rate::X1x1) {
                Check(ocu_vrs_guard::CurrentReasons(context.Get())==ocu_vrs_guard::Compatible,"original opaque shader restored without exact tag");
                const auto opaque=draw();Check(opaque<width*height*.8,"ordinary VRS resumes immediately after mask replay");
                std::printf("OPAQUE_RESTORED format=%s rate=%s PS=%llu\n",d24?"D24S8":"D32",ocu_foveation::RateName(rate),opaque);
            }
        }
    }
    ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();
}
int main() try {
    Run(false);Run(true);
    std::puts("PASS: production exact-pixel tag restores D24/D32 DAPA masks in both eyes, preserves scene, resumes ordinary VRS; tag storage failures refuse initialization");return 0;
} catch(const std::exception& e) {std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
