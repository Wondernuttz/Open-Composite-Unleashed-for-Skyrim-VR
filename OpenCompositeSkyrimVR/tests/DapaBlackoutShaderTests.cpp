#include "DrvOpenXR/DapaWarpShader.h"
#include "DrvOpenXR/DapaMotion.h"
#include "OpenOVR/Misc/FoveationBlackout.h"
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel=std::array<float,4>;
static void Check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
static void HR(HRESULT result){Check(SUCCEEDED(result),"D3D11 call failed");}
constexpr UINT Width=512,Height=256;
struct WarpConstants {
    float pose[16]{};
    float resolution[2]{float(Width),float(Height)},nearZ=1,farZ=100;
    float fov[4]{-1,1,1,-1};
    float depthScale=1,edgeFade=0,nearFade=0,tint=0;
    float depthSize[2]{float(Width),float(Height)},flip[2]{};
    float projection[16]{},inverse[16]{},valid=1,padding[3]{2,0,0};
};
struct BlackoutConstants {
    float center[2]{},inner=0,middle=0,scale=0,cutoff=0;
    uint32_t flags=0,enabled=0;
};
static_assert(sizeof(WarpConstants)==272);
static_assert(sizeof(BlackoutConstants)==32);

static void GuardChecks() {
    ocu_foveation::BlackoutFrame frame;
    frame.frameId=42;frame.mask.cutoff=true;frame.mask.guardPixels=260;
    for(auto& size:frame.sceneEyeSize){size[0]=2048;size[1]=1024;}
    Check(ocu_foveation::BlackoutSupportsDapaSource(frame,0,4096,2048),"adequate scaled scene guard rejected");
    Check(ocu_foveation::BlackoutSupportsDapaSource(frame,1,2048,1024),"adequate native scene guard rejected");
    frame.mask.guardPixels=16;
    Check(!ocu_foveation::BlackoutSupportsDapaSource(frame,0,4096,2048),"small guard admitted bounded warp reads");
    frame.mask.guardPixels=260;
    Check(!ocu_foveation::BlackoutSupportsDapaSource(frame,0,128,128),"64-pixel foreground seed bound omitted");
    frame.mask.guardPixels=float((.12+.5/4096.0)*2048);
    Check(!ocu_foveation::BlackoutSupportsDapaSource(frame,0,4096,2048),"guard boundary lacks floating-point margin");
    frame.mask.guardPixels=260;frame.sceneEyeSize[1][0]=0;
    Check(!ocu_foveation::BlackoutSupportsDapaSource(frame,1,4096,2048),"unknown scene dimensions admitted Cull cache");
    Check(!ocu_foveation::BlackoutSupportsDapaSource(frame,0,0,2048),"zero source dimensions admitted Cull cache");
    frame.mask={};
    Check(ocu_foveation::BlackoutSupportsDapaSource(frame,0,0,0),"ordinary DAPA cache gained blackout requirements");
    std::puts("CPU source guard: iteration bound, 64-pixel seeds, bilinear support, margin, scaled dimensions PASS");
}

class Fixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader[2];ComPtr<ID3D11SamplerState> sampler;
public:
    size_t poisonedSamples=0;
    explicit Fixture(D3D_DRIVER_TYPE driver) {
        HR(D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        const D3D_SHADER_MACRO capture[]={{"DAPA_CAPTURE","1"},{nullptr,nullptr}};
        for(unsigned variant=0;variant<2;++variant) {
            ComPtr<ID3DBlob> code,error;
            HRESULT result=D3DCompile(s_warpShaderHLSL,std::strlen(s_warpShaderHLSL),"DapaBlackoutProduction",
                variant?capture:nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_PACK_MATRIX_ROW_MAJOR,0,&code,&error);
            if(FAILED(result)&&error)std::fprintf(stderr,"%s\n",static_cast<const char*>(error->GetBufferPointer()));HR(result);
            ComPtr<ID3D11ShaderReflection> reflection;
            HR(D3DReflect(code->GetBufferPointer(),code->GetBufferSize(),IID_PPV_ARGS(&reflection)));
            D3D11_SHADER_BUFFER_DESC base{},blackout{};
            HR(reflection->GetConstantBufferByName("WarpParams")->GetDesc(&base));
            HR(reflection->GetConstantBufferByName("BlackoutParams")->GetDesc(&blackout));
            D3D11_SHADER_INPUT_BIND_DESC binding{};HR(reflection->GetResourceBindingDescByName("BlackoutParams",&binding));
            Check(base.Size==272&&blackout.Size==32&&binding.BindPoint==1,"shader constant ABI changed");
            HR(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader[variant]));
        }
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;desc.MaxLOD=D3D11_FLOAT32_MAX;
        HR(device->CreateSamplerState(&desc,&sampler));
    }
    std::array<std::vector<Pixel>,3> Run(const WarpConstants& params,const ocu_foveation::BlackoutFrame& frame,
        bool enabled,bool poison,bool capture) {
        context->ClearState();
        const float centerY=params.flip[1]>.5f?1-frame.centers[0][1]:frame.centers[0][1];
        BlackoutConstants mask{{frame.centers[0][0],centerY},frame.inner,frame.middle,frame.horizontalScale,
            float(ocu_foveation::BlackoutCutoff(frame.mask,frame.middle)),
            (frame.mask.middle?1u:0u)|(frame.mask.outer?2u:0u)|(frame.mask.cutoff?4u:0u),enabled?1u:0u};
        std::vector<Pixel> colors(Width*Height);std::vector<float> depth(Width*Height);
        for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x) {
            colors[size_t(y)*Width+x]={float(x)/Width,float(y)/Height,float((x/7+y/11)&1),1};
            const float z=x>Width/3&&x<Width*2/3?4.f:12.f;
            depth[size_t(y)*Width+x]=(params.projection[10]*z+params.projection[14])/z;
        }
        if(poison)for(UINT ty=0;ty<Height;ty+=16)for(UINT tx=0;tx<Width;tx+=16) {
            if(!ocu_foveation::WholeBlackoutTile(frame.mask,frame.centers[0][0],frame.centers[0][1],
                frame.inner,frame.middle,frame.horizontalScale,tx,ty,16,16,Width,Height))continue;
            for(UINT y=ty;y<ty+16;++y)for(UINT x=tx;x<tx+16;++x) {
                colors[size_t(y)*Width+x]={1000,1000,1000,1};++poisonedSamples;
            }
        }
        D3D11_TEXTURE2D_DESC desc{};desc.Width=Width;desc.Height=Height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{colors.data(),Width*sizeof(Pixel),0};ComPtr<ID3D11Texture2D> color,depthTex,output[3],read;
        HR(device->CreateTexture2D(&desc,&data,&color));ComPtr<ID3D11ShaderResourceView> colorView,depthView;
        HR(device->CreateShaderResourceView(color.Get(),nullptr,&colorView));
        desc.Format=DXGI_FORMAT_R32_FLOAT;data={depth.data(),Width*sizeof(float),0};
        HR(device->CreateTexture2D(&desc,&data,&depthTex));HR(device->CreateShaderResourceView(depthTex.Get(),nullptr,&depthView));
        desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11UnorderedAccessView> views[3];
        for(unsigned i=0;i<(capture?3u:1u);++i){HR(device->CreateTexture2D(&desc,nullptr,&output[i]));HR(device->CreateUnorderedAccessView(output[i].Get(),nullptr,&views[i]));}
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc,nullptr,&read));
        ComPtr<ID3D11Buffer> b0,b1;D3D11_BUFFER_DESC bd{};bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.ByteWidth=sizeof(params);
        data={&params,0,0};HR(device->CreateBuffer(&bd,&data,&b0));bd.ByteWidth=sizeof(mask);data={&mask,0,0};HR(device->CreateBuffer(&bd,&data,&b1));
        ID3D11ShaderResourceView* srvs[]={colorView.Get(),nullptr,depthView.Get()};
        ID3D11UnorderedAccessView* uavs[]={views[0].Get(),views[1].Get(),views[2].Get()};ID3D11Buffer* buffers[]={b0.Get(),b1.Get()};
        context->CSSetShader(shader[capture?1:0].Get(),nullptr,0);context->CSSetShaderResources(0,3,srvs);
        context->CSSetUnorderedAccessViews(0,capture?3:1,uavs,nullptr);context->CSSetConstantBuffers(0,2,buffers);
        context->CSSetSamplers(0,1,sampler.GetAddressOf());context->Dispatch((Width+7)/8,(Height+7)/8,1);
        std::array<std::vector<Pixel>,3> result;
        context->ClearState();
        for(unsigned i=0;i<(capture?3u:1u);++i) {
            context->CopyResource(read.Get(),output[i].Get());D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
            result[i].resize(Width*Height);
            for(UINT y=0;y<Height;++y)std::memcpy(result[i].data()+size_t(y)*Width,static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch,Width*sizeof(Pixel));
            context->Unmap(read.Get(),0);
        }
        return result;
    }
};

static void ShaderChecks(D3D_DRIVER_TYPE driver) {
    Fixture fixture(driver);size_t black=0,visible=0;
    for(unsigned profile=0;profile<3;++profile) {
        WarpConstants params;for(int i=0;i<4;++i)params.pose[i*4+i]=1;
        params.pose[3]=profile==2?0:.8f;params.pose[7]=.15f;params.valid=profile==2?0.f:1.f;
        params.flip[1]=profile==1?1.f:0.f;params.tint=profile==2?1.f:0.f;
        params.projection[0]=params.projection[5]=1;params.projection[10]=100.f/99;params.projection[11]=1;params.projection[14]=-100.f/99;
        Check(DapaMotion::Inverse(params.projection,params.inverse),"projection inverse failed");
        ocu_foveation::BlackoutFrame frame;frame.frameId=7;frame.inner=.2f;frame.middle=.55f;
        frame.centers[0][0]=.35f;frame.centers[0][1]=.37f;frame.horizontalScale=profile==1?.65f:profile==2?1.7f:1.f;
        frame.sceneEyeSize[0][0]=Width;frame.sceneEyeSize[0][1]=Height;
        frame.mask.guardPixels=66;frame.mask.cutoffRadius=.8f;
        const bool capture=profile==2;
        const auto baseline=fixture.Run(params,frame,false,false,capture);
        for(unsigned flags=1;flags<8;++flags) {
            frame.mask.middle=(flags&1)!=0;frame.mask.outer=(flags&2)!=0;frame.mask.cutoff=(flags&4)!=0;
            Check(ocu_foveation::BlackoutSupportsDapaSource(frame,0,Width,Height),"fixture source guard insufficient");
            const auto masked=fixture.Run(params,frame,true,true,capture);
            for(UINT y=0;y<Height;++y)for(UINT x=0;x<Width;++x) {
                const float cy=params.flip[1]>.5f?1-frame.centers[0][1]:frame.centers[0][1];
                const float dx=2*((x+.5f)/Width-frame.centers[0][0])/frame.horizontalScale;
                const float dy=2*((y+.5f)/Height-cy);const float radius=std::sqrt(dx*dx+dy*dy);
                const bool hidden=(frame.mask.middle&&radius>frame.inner&&radius<=frame.middle)||
                    (frame.mask.outer&&radius>frame.middle)||(frame.mask.cutoff&&radius>std::max(frame.middle,frame.mask.cutoffRadius));
                const size_t i=size_t(y)*Width+x;
                for(unsigned output=0;output<(capture?3u:1u);++output) {
                    if(hidden) {
                        const Pixel expected=output==2?Pixel{0,.5f,.5f,1}:Pixel{0,0,0,1};
                        Check(masked[output][i]==expected,"black target/capture not opaque or tinted incorrectly");
                    } else Check(std::memcmp(&masked[output][i],&baseline[output][i],sizeof(Pixel))==0,"guarded Cull changed visible warp output");
                }
                if(hidden)++black;else ++visible;
            }
        }
        const auto disabled=fixture.Run(params,frame,false,false,capture);
        Check(std::memcmp(disabled[0].data(),baseline[0].data(),baseline[0].size()*sizeof(Pixel))==0,"disabling blackout altered ordinary warp");
    }
    Check(fixture.poisonedSamples>1000,"no culled source pixels exercised");
    std::printf("PASS %s: 272/32-byte ABI; all flags, asymmetric/elliptical/flipped masks, pose warp, guarded poison, capture/tint, disabled recovery; black=%zu visible=%zu poisonedSources=%zu\n",
        driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware",black,visible,fixture.poisonedSamples);
}

int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        GuardChecks();ShaderChecks(D3D_DRIVER_TYPE_WARP);
        if(argc>1&&std::string(argv[1])=="hardware")ShaderChecks(D3D_DRIVER_TYPE_HARDWARE);
        return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
