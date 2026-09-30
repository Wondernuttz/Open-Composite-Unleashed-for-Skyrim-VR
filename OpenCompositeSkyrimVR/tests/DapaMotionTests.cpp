#include "DrvOpenXR/DapaMotion.h"
#include "DrvOpenXR/DapaWarpShader.h"
namespace LegacyShader {
#include "fixtures/DapaWarpPreBoundary.h"
}
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include "DrvOpenXR/DapaPlayerMaskGpu.h"
using Microsoft::WRL::ComPtr;
static void Check(bool b,const char* s) { if(!b) throw std::runtime_error(s); }
static void HR(HRESULT h) { Check(SUCCEEDED(h),"D3D11 operation failed"); }
static bool Near(float a,float b,float eps=0.002f) { return std::abs(a-b)<eps; }
struct Constants {
    float transform[16]{};
    float resolution[2]{32,16}, nearZ=1,farZ=100;
    float fov[4]{-1,1,1,-1};
    float depthScale=1,edgeFade=3,nearFade=0,tint=0;
    float depthSize[2]{32,16},flip[2]{};
    float p[16]{},invP[16]{},valid=1,pad[3]{};
};
static_assert(sizeof(Constants)==272);
static void Identity(float* p) { std::fill(p,p+16,0.0f); for(int i=0;i<4;++i)p[i*4+i]=1; }
static void Projection(float* p,bool reversed,float asym=0) {
    std::fill(p,p+16,0.0f);
    p[0]=p[5]=1; p[8]=asym; p[11]=1;
    p[10]=reversed ? -1.0f/99 : 100.0f/99;
    p[14]=reversed ? 100.0f/99 : -100.0f/99;
}
static float Depth(const Constants& c,float z) {
    if(c.p[11]<0) z=-z;
    return (c.p[10]*z+c.p[14])/(c.p[11]*z+c.p[15]);
}
class GpuTest {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> sampler;
public:
    GpuTest(bool repaired=false) {
        D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx));
        ComPtr<ID3DBlob> blob,errors;
        const char* source=repaired?s_warpShaderHLSL:LegacyShader::s_warpShaderHLSL;
        HRESULT h=D3DCompile(source,strlen(source),"DapaProductionWarp",nullptr,nullptr,
            "CSMain","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
        if(FAILED(h)&&errors) std::fprintf(stderr,"%s\n",(char*)errors->GetBufferPointer());
        HR(h); HR(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(Constants); bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        HR(device->CreateBuffer(&bd,nullptr,&cb));
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        HR(device->CreateSamplerState(&sd,&sampler));
    }
    std::vector<std::array<float,4>> Run(Constants c,bool edge=false,int depthDiv=1,bool rectangle=false,int bodyMode=0) {
        c.depthSize[0]=32.0f/depthDiv; c.depthSize[1]=16.0f/depthDiv;
        std::vector<std::array<float,4>> color(32*16);
        for(int y=0;y<16;++y) for(int x=0;x<32;++x) color[y*32+x]={float(x)/31,float(y)/15,0,1};
        if(rectangle)for(int y=0;y<16;++y)for(int x=0;x<32;++x)color[y*32+x]=x>=8&&x<24?std::array<float,4>{1,0,0,1}:std::array<float,4>{0,0,1,1};
        const int dw=32/depthDiv,dh=16/depthDiv;
        std::vector<float> depth(dw*dh);
        for(int y=0;y<dh;++y) for(int x=0;x<dw;++x) depth[y*dw+x]=Depth(c,edge&&x>=dw/2?20.0f:10.0f);
        if(rectangle)for(int y=0;y<dh;++y)for(int x=0;x<dw;++x)depth[y*dw+x]=Depth(c,x>=8/depthDiv&&x<24/depthDiv?5.0f:20.0f);
        D3D11_TEXTURE2D_DESC td{}; td.Width=32;td.Height=16;td.MipLevels=td.ArraySize=1;
        td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{color.data(),32*16,0};
        ComPtr<ID3D11Texture2D> tc,tdp,to,read;
        HR(device->CreateTexture2D(&td,&data,&tc));
        td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;HR(device->CreateTexture2D(&td,nullptr,&to));
        td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&td,nullptr,&read));
        td.Width=dw;td.Height=dh;td.Format=DXGI_FORMAT_R32_FLOAT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;
        data={depth.data(),UINT(dw*4),0};HR(device->CreateTexture2D(&td,&data,&tdp));
        ComPtr<ID3D11ShaderResourceView> sc,sd,bodySrv;
        ComPtr<ID3D11Texture2D> bodyTex;
        if(bodyMode) {
            std::vector<float> body(dw*dh,-1);
            for(int y=0;y<dh;++y)for(int x=8/depthDiv;x<24/depthDiv;++x)
                body[y*dw+x]=bodyMode==2?Depth(c,3.0f):depth[y*dw+x];
            data={body.data(),UINT(dw*4),0};HR(device->CreateTexture2D(&td,&data,&bodyTex));
            HR(device->CreateShaderResourceView(bodyTex.Get(),nullptr,&bodySrv));c.pad[0]=3;
        }
        ComPtr<ID3D11UnorderedAccessView> out;
        HR(device->CreateShaderResourceView(tc.Get(),nullptr,&sc));HR(device->CreateShaderResourceView(tdp.Get(),nullptr,&sd));
        HR(device->CreateUnorderedAccessView(to.Get(),nullptr,&out));
        ctx->UpdateSubresource(cb.Get(),0,nullptr,&c,0,0);
        ID3D11ShaderResourceView* srvs[]={sc.Get(),bodySrv.Get(),sd.Get()};
        ID3D11UnorderedAccessView* uavs[]={out.Get()};
        ID3D11Buffer* buffers[]={cb.Get()};ID3D11SamplerState* samplers[]={sampler.Get()};
        ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,3,srvs);
        ctx->CSSetUnorderedAccessViews(0,1,uavs,nullptr);ctx->CSSetConstantBuffers(0,1,buffers);
        ctx->CSSetSamplers(0,1,samplers);ctx->Dispatch(4,2,1);
        ID3D11UnorderedAccessView* nu[]={nullptr};ctx->CSSetUnorderedAccessViews(0,1,nu,nullptr);
        ID3D11ShaderResourceView* ns[]={nullptr,nullptr,nullptr};ctx->CSSetShaderResources(0,3,ns);
        ctx->CopyResource(read.Get(),to.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(int y=0;y<16;++y) std::memcpy(color.data()+y*32,(char*)mapped.pData+y*mapped.RowPitch,32*16);
        ctx->Unmap(read.Get(),0);
        for(auto pixel:color)for(float v:pixel)Check(std::isfinite(v),"non-finite shader output");
        return color;
    }
};
static void MaskRenderStateTest() {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&level,&ctx));
    DapaPlayerMaskGpu mask;Check(mask.Initialize(d.Get())&&mask.Size(d.Get(),16,16),"mask GPU initialization");
    mask.Clear(ctx.Get());
    const char* code="float4 main(uint id:SV_VertexID):SV_Position { float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0.4,1); }";
    ComPtr<ID3DBlob> blob;HR(D3DCompile(code,strlen(code),"mask-test",nullptr,nullptr,"main","vs_5_0",0,0,&blob,nullptr));
    ComPtr<ID3D11VertexShader> vs;HR(d->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&vs));
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> scene,uavTex,read;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    HR(d->CreateTexture2D(&td,nullptr,&scene));HR(d->CreateRenderTargetView(scene.Get(),nullptr,&rtv));
    td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;HR(d->CreateTexture2D(&td,nullptr,&uavTex));HR(d->CreateUnorderedAccessView(uavTex.Get(),nullptr,&uav));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(d->CreateTexture2D(&td,nullptr,&read));
    const float clear[4]={.9f,.9f,.9f,.9f};ctx->ClearRenderTargetView(rtv.Get(),clear);
    auto* rt=rtv.Get();auto* ua=uav.Get();UINT counter=0xffffffff;
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(1,&rt,nullptr,3,1,&ua,&counter);
    D3D11_BLEND_DESC bd{};bd.RenderTarget[0].RenderTargetWriteMask=0;
    ComPtr<ID3D11BlendState> blend;HR(d->CreateBlendState(&bd,&blend));
    float factors[4]={.1f,.2f,.3f,.4f};ctx->OMSetBlendState(blend.Get(),factors,0x12345678);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(d->CreateRasterizerState(&rd,&raster));ctx->RSSetState(raster.Get());
    D3D11_VIEWPORT viewport{0,0,8,16,0,1};ctx->RSSetViewports(1,&viewport);
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    mask.Replay(ctx.Get(),[&]{ctx->Draw(3,0);});
    ComPtr<ID3D11RenderTargetView> gotRT;ComPtr<ID3D11UnorderedAccessView> gotUAV;
    ctx->OMGetRenderTargetsAndUnorderedAccessViews(1,&gotRT,nullptr,3,1,&gotUAV);
    Check(gotRT.Get()==rt && gotUAV.Get()==ua,"mask restores CSX render targets and UAVs");
    ComPtr<ID3D11BlendState> gotBlend;FLOAT gotFactors[4];UINT gotSamples;
    ctx->OMGetBlendState(&gotBlend,gotFactors,&gotSamples);
    Check(gotBlend.Get()==blend.Get() && gotSamples==0x12345678 && Near(gotFactors[2],.3f),"mask restores blend factors and sample mask");
    ctx->CopyResource(read.Get(),mask.Texture());D3D11_MAPPED_SUBRESOURCE mapped{};
    HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        float value;memcpy(&value,(char*)mapped.pData+y*mapped.RowPitch+x*4,4);
        Check(Near(value,x<8?.4f:-1.0f),"mask writes raw depth only in active eye viewport");
    }
    ctx->Unmap(read.Get(),0);ctx->CopyResource(read.Get(),scene.Get());HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    for(int y=0;y<16;++y)for(int x=0;x<16;++x) {
        float value;memcpy(&value,(char*)mapped.pData+y*mapped.RowPitch+x*4,4);Check(Near(value,.9f),"mask never modifies game colour");
    }
    ctx->Unmap(read.Get(),0);
}
int main() {
    try {
        MaskRenderStateTest();
        for(double hz:{60.,72.,80.,90.,96.,100.,120.,144.}) {
            DapaMotion::YawPredictor turn;
            const int64_t turnStep=int64_t(2e9/hz),turnStart=1000000000;
            const float yawStep=float(turnStep*1e-9);
            turn.Sample(turnStart,0,false);
            turn.Sample(turnStart+turnStep,yawStep,true);
            Check(turn.Predict(turnStart+turnStep+turnStep/2)==0,"turn onset needs rate history");
            turn.Sample(turnStart+2*turnStep,2*yawStep,true);
            Check(Near(turn.Predict(turnStart+2*turnStep+turnStep/2),-yawStep/2),"stationary actor turn has independent clock");
            Check(Near(turn.Predict(turnStart+2*turnStep+turnStep/4),-yawStep/4),"turn uses actual prediction horizon");
            Check(turn.Predict(turnStart+4*turnStep)==0,"stale turn rejected");
            turn.Sample(turnStart+3*turnStep,3*yawStep,false);
            Check(turn.Predict(turnStart+3*turnStep+turnStep/2)==0,"released turn cannot coast");
            turn.Sample(turnStart+4*turnStep,4*yawStep,true);
            turn.Sample(turnStart+5*turnStep,3*yawStep,true);
            Check(turn.confidence==0,"turn reversal suppresses overshoot");
            turn.Sample(turnStart+6*turnStep,3*yawStep+1,true);
            Check(!turn.haveRate,"snap turn is not extrapolated");
            turn.Sample(turnStart+20*turnStep,0,true);Check(!turn.haveRate,"turn gap rejected");
            turn.Sample(turnStart+20*turnStep,0.1f,true);Check(!turn.haveRate,"duplicate turn timestamp rejected");
            turn.Sample(0,0,true);Check(!turn.haveYaw,"missing actor heading resets turn");
            turn.Sample(turnStart,3.13f,true);turn.Sample(turnStart+turnStep,-3.13f,true);
            Check(std::abs(turn.delta)<0.03f,"heading wrap is short arc, not snap");
            DapaMotion::Predictor p;
            const int64_t step=int64_t(2e9/hz),start=1000000000;
            const float move=float(step*1e-9*100);
            p.Sample(start,{0,0,0});p.Sample(start+step,{move,0,0});
            Check(p.confidence==0,"startup must not trust a single velocity sample");
            p.Sample(start+2*step,{2*move,0,0});
            Check(Near(p.Predict(start+2*step+step/2).x,move/2),"timestamp-scaled constant motion");
            Check(Near(p.Predict(start+2*step+step/4).x,move/4),"quarter slot not hardcoded half");
            Check(p.Predict(start+4*step).x==0,"stale prediction rejected");
            p.Sample(start+3*step,{2*move,0,0});
            Check(p.Predict(start+3*step+step/2).x==0,"stop must not coast");
            p.Sample(start+4*step,{move,0,0});
            Check(p.confidence==0,"reversal / restart lowers confidence");
            p.Sample(start+5*step,{1000,0,0});Check(p.confidence==0,"teleport reset");
            p.Sample(start+1000000000,{1001,0,0});Check(p.confidence==0,"gap reset");
            p.Sample(start+1000000000,{1002,0,0});Check(p.confidence==0,"duplicate timestamp");
            p.Sample(start+1000000001,{std::numeric_limits<float>::quiet_NaN(),0,0});
            Check(!p.havePosition,"invalid input reset");
        }
        float view[16],vp[16],projection[16],inverse[16];Identity(view);
        view[0]=view[5]=view[10]=2;
        Projection(projection,true,0.13f);
        for(int r=0;r<4;++r)for(int c=0;c<4;++c){vp[r*4+c]=0;for(int k=0;k<4;++k)vp[r*4+c]+=view[r*4+k]*projection[k*4+c];}
        float recovered[16];Check(DapaMotion::Projection(view,vp,recovered,inverse),"projection recovery");
        for(int i=0;i<16;++i)Check(Near(recovered[i],projection[i]),"projection preserves world scale / asymmetric FOV");
        Check(Near(DapaMotion::ToView({1,0,0},view).x,2),"view world scale preserved");
        float singular[16]{};Check(!DapaMotion::Projection(singular,vp,recovered,inverse),"singular matrix rejected");
        for(bool repaired:{false,true}) {
        GpuTest gpu(repaired);
        for(bool reversed:{false,true})for(float asym:{-0.13f,0.13f})for(int div:{1,2})for(bool rh:{false,true}) {
            Constants c;Identity(c.transform);Projection(c.p,reversed,asym);
            if(rh)for(int i=8;i<12;++i)c.p[i]=-c.p[i];
            Check(DapaMotion::Inverse(c.p,c.invP),"projection inverse");
            auto identity=gpu.Run(c,false,div);
            Check(Near(identity[8*32+12][0],12.0f/31),"identity warp");
            c.transform[3]=0.5f; // backward lookup shifts right for camera moving right
            auto right=gpu.Run(c,false,div);
            Check(right[8*32+12][0]>identity[8*32+12][0]+0.015f,"strafe direction");
            c.transform[3]=-0.5f;
            auto left=gpu.Run(c,false,div);
            Check(left[8*32+12][0]<identity[8*32+12][0]-0.015f,"reverse strafe direction");
            c.transform[3]=0.5f;
            auto edge=gpu.Run(c,true,div);
            if(!repaired)Check(Near(edge[8*32+15][0],15.0f/31),"depth discontinuity rejects foreground smear");
            c.valid=0;auto invalid=gpu.Run(c,false,div);
            Check(Near(invalid[8*32+12][0],12.0f/31),"invalid geometry uses unchanged real cache");
            c.valid=1;c.transform[3]=0;c.flip[1]=1;
            auto flip=gpu.Run(c,false,div);
            Check(Near(flip[3*32+12][1],12.0f/15),"reversed V bounds");
            c.flip[1]=0;c.transform[11]=rh?-0.1f:0.1f;
            auto forward=gpu.Run(c,false,div);
            Check(forward[8*32+24][0]<identity[8*32+24][0],"forward projection contracts lookup");
            if(repaired) {
                // Looking world +Y; +Z is up. Actor positive yaw turns right.
                // NEW->OLD world rotation for a right turn is negative Z.
                float eyeView[16]{};eyeView[0]=1;eyeView[9]=1;eyeView[6]=rh?-1.0f:1.0f;eyeView[15]=1;
                Identity(c.transform);
                Check(DapaMotion::WorldYawToView(-0.04f,eyeView,c.transform),"world yaw in each eye basis");
                auto turnRight=gpu.Run(c,false,div);
                Check(turnRight[8*32+12][0]>identity[8*32+12][0]+0.015f,"right turn samples right, in both handedness");
                Check(DapaMotion::WorldYawToView(0.04f,eyeView,c.transform),"reverse world yaw");
                auto turnLeft=gpu.Run(c,false,div);
                Check(turnLeft[8*32+12][0]<identity[8*32+12][0]-0.015f,"left turn samples left");
                for(float angle:{-0.1f,0.1f}) {
                    Check(DapaMotion::WorldYawToView(angle,eyeView,c.transform),"outer edge turn matrix");
                    const auto edgeTurn=gpu.Run(c,false,div);
                    const int x=angle<0?30:1;
                    Check(Near(edgeTurn[8*32+x][0],angle<0?1.0f:0.0f,0.003f),"exposed turn edge extends boundary, never repeats unwarped strip");
                    for(int col=1;col<32;++col)Check(edgeTurn[8*32+col][0]+0.003f>=edgeTurn[8*32+col-1][0],"turn boundary cannot jump back into old image");
                    c.flip[1]=1;
                    const auto flippedEdge=gpu.Run(c,false,div);
                    Check(Near(flippedEdge[8*32+x][0],angle<0?1.0f:0.0f,0.003f),"flipped eye bounds preserve exposed-edge fix");
                    c.flip[1]=0;
                }
                // Pitch the head: expected transform is world rotation followed by view,
                // not a guessed rotation about the screen's vertical axis.
                const float tilt=0.6f;
                eyeView[5]=std::sin(tilt);eyeView[9]=std::cos(tilt);
                eyeView[6]=std::cos(tilt)*(rh?-1:1);eyeView[10]=-std::sin(tilt)*(rh?-1:1);
                for(float& v:eyeView)v*=2;eyeView[15]=1;
                Check(DapaMotion::WorldYawToView(-0.04f,eyeView,c.transform),"tilted scaled view turn");
                const DapaMotion::Vec3 world{2,10,3};
                const auto point=DapaMotion::ToView(world,eyeView);
                const float ca=std::cos(-0.04f),sa=std::sin(-0.04f);
                const auto expected=DapaMotion::ToView({ca*world.x-sa*world.y,sa*world.x+ca*world.y,world.z},eyeView);
                const DapaMotion::Vec3 actual{c.transform[0]*point.x+c.transform[1]*point.y+c.transform[2]*point.z,
                    c.transform[4]*point.x+c.transform[5]*point.y+c.transform[6]*point.z,
                    c.transform[8]*point.x+c.transform[9]*point.y+c.transform[10]*point.z};
                Check(DapaMotion::Length(actual-expected)<0.001f,"world-up conjugation matches geometric reference");
                const auto tilted=gpu.Run(c,false,div);Check(tilted[8*32+12][0]>identity[8*32+12][0],"tilted turn direction");
                // Pure vertical edge exposure exercises top/bottom and flipped source bounds.
                for(float direction:{-1.0f,1.0f})for(float flipV:{0.0f,1.0f}) {
                    Identity(c.transform);c.transform[7]=direction*2.0f;c.flip[1]=flipV;
                    const auto vertical=gpu.Run(c,false,div);
                    const int y=direction>0?(flipV>0?15:0):(flipV>0?0:15);
                    Check(Near(vertical[y*32+12][1],direction>0?0.0f:1.0f,0.003f),"top/bottom boundary extension with flipped source");
                }
                c.flip[1]=0;
                Identity(c.transform);
                c.transform[11]=0;c.transform[3]=0.5f;c.nearFade=100;
                const auto nearProtected=gpu.Run(c,false,div);
                Check(Near(nearProtected[8*32+12][0],identity[8*32+12][0]),"configured near fade preserved");
                c.nearFade=0;c.depthScale=2;
                const auto scaled=gpu.Run(c,false,div);
                Check(scaled[8*32+12][0]>identity[8*32+12][0] && scaled[8*32+12][0]<right[8*32+12][0],"depth scale reduces parallax as configured");
                c.depthScale=1;
                c.transform[11]=0;
                for(float direction:{-1.0f,1.0f}) {
                    c.transform[3]=direction*0.9375f;
                    const auto silhouette=gpu.Run(c,false,div,true);
                    int wrong=0;
                    for(int x=2;x<30;++x){const bool foreground=(x+0.5f)>=8-direction*3 && (x+0.5f)<24-direction*3;
                        if((silhouette[8*32+x][0]>0.5f)!=foreground)++wrong;}
                    if(wrong>1){std::printf("silhouette mismatches=%d direction=%f reversed=%d asym=%f div=%d rh=%d\n",wrong,direction,reversed,asym,div,rh);for(int x=0;x<32;++x)std::printf("%.1f ",silhouette[8*32+x][0]);std::puts("");}
                    Check(wrong<=1,"foreground silhouette preserved under opposite strafes (not simply erased)");
                }
            }
        }
        }
        GpuTest bodyGpu(true);
        for(bool reversed:{false,true})for(int div:{1,2})for(float flip:{0.0f,1.0f})for(float direction:{-1.0f,1.0f}) {
            Constants c;Identity(c.transform);Projection(c.p,reversed);
            Check(DapaMotion::Inverse(c.p,c.invP),"body projection inverse");c.flip[1]=flip;
            const auto reference=bodyGpu.Run(c,false,div,true);
            for(int mode=0;mode<3;++mode) {
                Identity(c.transform);
                if(mode==0)c.transform[3]=direction*.9375f;
                if(mode==1)c.transform[11]=direction*.5f;
                if(mode==2){const float a=direction*.04f;c.transform[0]=c.transform[10]=std::cos(a);c.transform[2]=std::sin(a);c.transform[8]=-std::sin(a);}
                const auto world=bodyGpu.Run(c,false,div,true);
                const auto body=bodyGpu.Run(c,false,div,true,1);
                const auto hidden=bodyGpu.Run(c,false,div,true,2);
                for(int y=0;y<16;++y)for(int x=8;x<24;++x)
                    Check(Near(body[y*32+x][0],reference[y*32+x][0]),"player pixels retained through strafe/forward/turn");
                for(int i=0;i<512;++i)for(int channel=0;channel<4;++channel)
                    Check(Near(world[i][channel],hidden[i][channel],.00001f),"wrong-depth mask must not protect occluders");
                Check(body[8*32+3][2]>.99f && body[8*32+28][2]>.99f,"body mask does not erase nearby world");
            }
        }
        std::puts("PASS: existing world-motion regressions plus player mask: strafe, forward/back, yaw, reduced depth, flipped bounds, depth conventions and occlusion rejection.");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
