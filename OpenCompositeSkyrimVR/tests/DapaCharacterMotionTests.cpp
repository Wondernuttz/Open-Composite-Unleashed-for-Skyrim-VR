#include "DrvOpenXR/DapaNativePlayerMotion.h"
#include "DrvOpenXR/DapaCharacterTiming.h"
#include "DrvOpenXR/DapaComputeState.h"
#include "DrvOpenXR/DapaWarpShader.h"
#include <d3dcompiler.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <limits>
#include <chrono>
#include <thread>
using Microsoft::WRL::ComPtr;
static unsigned checks=0;
static void Check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static void HR(HRESULT h){Check(SUCCEEDED(h),"D3D11 operation failed");}
static void Identity(float* m){std::fill(m,m+16,0.f);for(int i=0;i<4;++i)m[i*5]=1;}
struct Warp {
    float transform[16]{},size[2]{64,32},nearZ=1,farZ=100,fov[4]{-1,1,1,-1};
    float scale=1,edge=0,nearFade=0,tint=0,depthSize[2]{64,32},flip[2]{};
    float projection[16]{},inverse[16]{},valid=0,pad[3]{3,0,0};
};
static_assert(sizeof(Warp)==272);
using Pixel=std::array<float,4>;
class Fixture {
public:
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> shader;ComPtr<ID3D11SamplerState> sampler;
    Fixture(D3D_DRIVER_TYPE type) {
        HR(D3D11CreateDevice(nullptr,type,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
        ComPtr<ID3DBlob> blob,error;
        auto result=D3DCompile(s_warpShaderHLSL,strlen(s_warpShaderHLSL),"NativePlayerProduction",nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
        if(FAILED(result)&&error)std::fprintf(stderr,"%s\n",(const char*)error->GetBufferPointer());
        HR(result);HR(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;desc.MaxLOD=D3D11_FLOAT32_MAX;
        HR(device->CreateSamplerState(&desc,&sampler));
    }
    ComPtr<ID3D11Texture2D> Texture(unsigned w,unsigned h,DXGI_FORMAT fmt,const void* data,unsigned stride,UINT bind=D3D11_BIND_SHADER_RESOURCE) {
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.Format=fmt;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA initial{data,stride,0};ComPtr<ID3D11Texture2D> t;
        HR(device->CreateTexture2D(&d,data?&initial:nullptr,&t));return t;
    }
    std::vector<Pixel> Run(DapaNativePlayerMotion::Constants native,float vx,float vy,bool flip=false,int depthDiv=1,bool occluded=false,float worldTranslation=0,DXGI_FORMAT mvFormat=DXGI_FORMAT_R32G32B32A32_FLOAT,bool discontinuous=false) {
        ctx->ClearState();
        Warp w;Identity(w.transform);Identity(w.projection);Identity(w.inverse);w.flip[1]=flip?1.f:0.f;
        const unsigned dw=64/depthDiv,dh=32/depthDiv;w.depthSize[0]=float(dw);w.depthSize[1]=float(dh);
        w.transform[3]=worldTranslation;w.valid=worldTranslation!=0?1.f:0.f;
        native.vectorSize[0]=float(dw);native.vectorSize[1]=float(dh);
        std::vector<Pixel> colors(64*32),vectors(dw*dh,Pixel{vx,vy,0,0});
        std::vector<float> depth(dw*dh,.5f),mask(dw*dh,-1.f);
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x)colors[y*64+x]={float(x)/64,float(y)/32,0,1};
        for(unsigned y=2/depthDiv;y<30/depthDiv;++y)for(unsigned x=16/depthDiv;x<48/depthDiv;++x)mask[y*dw+x]=occluded?.25f:.5f;
        auto color=Texture(64,32,DXGI_FORMAT_R32G32B32A32_FLOAT,colors.data(),64*16);
        auto dep=Texture(dw,dh,DXGI_FORMAT_R32_FLOAT,depth.data(),dw*4);
        auto body=Texture(dw,dh,DXGI_FORMAT_R32_FLOAT,mask.data(),dw*4);
        auto mv=Texture(dw,dh,mvFormat,nullptr,0,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> mvTarget;HR(device->CreateRenderTargetView(mv.Get(),nullptr,&mvTarget));
        const float vectorValue[4]={vx,vy,0,0};ctx->ClearRenderTargetView(mvTarget.Get(),vectorValue);
        if(discontinuous) {
            Check(mvFormat==DXGI_FORMAT_R32G32B32A32_FLOAT,"fixture format");
            for(unsigned y=0;y<dh;++y)for(unsigned x=0;x<dw;++x)
                vectors[y*dw+x]={x<dw/2?-vx:vx,vy,0,0};
            ctx->UpdateSubresource(mv.Get(),0,nullptr,vectors.data(),dw*16,0);
        }
        auto output=Texture(64,32,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,0,D3D11_BIND_UNORDERED_ACCESS);
        ComPtr<ID3D11ShaderResourceView> views[4];ID3D11Texture2D* inputs[]={color.Get(),body.Get(),dep.Get(),mv.Get()};
        for(int i=0;i<4;++i)HR(device->CreateShaderResourceView(inputs[i],nullptr,&views[i]));
        ComPtr<ID3D11UnorderedAccessView> out;HR(device->CreateUnorderedAccessView(output.Get(),nullptr,&out));
        ComPtr<ID3D11Buffer> buffers[3];const float blackout[8]{};
        const void* data[]={&w,blackout,&native};const unsigned sizes[]={sizeof(w),sizeof(blackout),sizeof(native)};
        for(int i=0;i<3;++i){D3D11_BUFFER_DESC d{};d.ByteWidth=sizes[i];d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA s{data[i],0,0};HR(device->CreateBuffer(&d,&s,&buffers[i]));}
        // Exercise the new t3/b2 slots against non-default external bindings.
        ctx->CSSetShaderResources(3,1,views[0].GetAddressOf());ctx->CSSetConstantBuffers(2,1,buffers[0].GetAddressOf());
        {
            DapaComputeState restore(ctx.Get(),false);
            ID3D11ShaderResourceView* raw[]={views[0].Get(),views[1].Get(),views[2].Get(),views[3].Get()};
            ID3D11Buffer* cbs[]={buffers[0].Get(),buffers[1].Get(),buffers[2].Get()};
            ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,3,raw);
            ctx->CSSetConstantBuffers(0,2,cbs);ctx->CSSetSamplers(0,1,sampler.GetAddressOf());
            ctx->CSSetUnorderedAccessViews(0,1,out.GetAddressOf(),nullptr);ctx->Dispatch(8,4,1);
        }
        ComPtr<ID3D11ShaderResourceView> restored;ComPtr<ID3D11Buffer> restoredCB;
        ctx->CSGetShaderResources(3,1,&restored);ctx->CSGetConstantBuffers(2,1,&restoredCB);
        Check(restored==views[0]&&restoredCB==buffers[0],"native slots did not restore");
        if(native.enabled>.5) {
            DapaNativePlayerMotion state;
            DapaNativePlayerMotion::Camera camera{};Identity(camera.jittered);
            std::copy(native.clipToPrevious,native.clipToPrevious+16,camera.current);
            std::copy(native.clipToPrevious,native.clipToPrevious+16,camera.previous);
            D3D11_BOX region{0,0,0,dw,dh,1};
            for(int frame=1;frame<=2;++frame) {
                if(frame==2)std::copy(native.clipToCurrent,native.clipToCurrent+16,camera.current);
                state.BeginPair();
                for(int eye=0;eye<2;++eye) {
                    Check(state.Capture(eye,ctx.Get(),mv.Get(),&region,dw,dh,views[1].Get(),views[2].Get(),64,32,flip),"isolated capture failed");
                    state.Observe(eye,camera,frame,1000000000LL+(frame-1)*20000000LL);
                }
            }
            auto foreign=Texture(8,8,DXGI_FORMAT_R32G32B32A32_FLOAT,nullptr,0,D3D11_BIND_UNORDERED_ACCESS);
            ComPtr<ID3D11UnorderedAccessView> foreignUav;HR(device->CreateUnorderedAccessView(foreign.Get(),nullptr,&foreignUav));
            ctx->CSSetUnorderedAccessViews(2,1,foreignUav.GetAddressOf(),nullptr);
            Check(state.Apply(0,ctx.Get(),1030000000LL,views[0].Get(),views[1].Get(),views[2].Get(),out.Get()),"isolated Apply failed");
            ComPtr<ID3D11UnorderedAccessView> restoredUav;ctx->CSGetUnorderedAccessViews(2,1,&restoredUav);
            Check(restoredUav==foreignUav,"prepared motion clobbered producer u2");
            ID3D11UnorderedAccessView* empty=nullptr;ctx->CSSetUnorderedAccessViews(2,1,&empty,nullptr);
            // Higher slots touched only by the isolated pass must also restore.
            ComPtr<ID3D11ShaderResourceView> again;ctx->CSGetShaderResources(3,1,&again);
            Check(again==views[0],"character pass clobbered external t3");
        }
        D3D11_TEXTURE2D_DESC d{};output->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read;HR(device->CreateTexture2D(&d,nullptr,&read));ctx->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::vector<Pixel> pixels(64*32);for(unsigned y=0;y<32;++y)memcpy(pixels.data()+y*64,(char*)mapped.pData+y*mapped.RowPitch,64*16);
        ctx->Unmap(read.Get(),0);return pixels;
    }
    void Benchmark(bool sparse=false) {
        const unsigned dw=2048,dh=2176,ow=3072,oh=3264;
        std::vector<Pixel> vectors(size_t(dw)*dh,Pixel{-4.f/ow,0,0,0});
        std::vector<float> depths(size_t(dw)*dh,.5f),mask(size_t(dw)*dh,-1.f);
        for(unsigned y=dh/4;y<dh*3/4;++y)for(unsigned x=dw/4;x<dw*3/4;++x)mask[size_t(y)*dw+x]=.5f;
        auto mv=Texture(dw,dh,DXGI_FORMAT_R16G16_FLOAT,nullptr,0,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET);
        ComPtr<ID3D11RenderTargetView> mvTarget;HR(device->CreateRenderTargetView(mv.Get(),nullptr,&mvTarget));
        const float vectorValue[4]={-4.f/ow,0,0,0};ctx->ClearRenderTargetView(mvTarget.Get(),vectorValue);
        if(sparse) {
            const float zero[4]{};ctx->ClearRenderTargetView(mvTarget.Get(),zero);
            ComPtr<ID3D11DeviceContext1> ctx1;HR(ctx.As(&ctx1));
            const D3D11_RECT rect{LONG(dw*7/16),LONG(dh*3/8),LONG(dw*9/16),LONG(dh*5/8)};
            ctx1->ClearView(mvTarget.Get(),vectorValue,&rect,1);
        }
        auto depth=Texture(dw,dh,DXGI_FORMAT_R32_FLOAT,depths.data(),dw*4);
        auto body=Texture(dw,dh,DXGI_FORMAT_R32_FLOAT,mask.data(),dw*4);
        auto color=Texture(ow,oh,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,0);
        ComPtr<ID3D11ShaderResourceView> views[3];ID3D11Texture2D* ts[]={color.Get(),body.Get(),depth.Get()};
        for(int i=0;i<3;++i)HR(device->CreateShaderResourceView(ts[i],nullptr,&views[i]));
        ComPtr<ID3D11Texture2D> outputs[2];ComPtr<ID3D11UnorderedAccessView> uavs[2];
        for(int i=0;i<2;++i){outputs[i]=Texture(ow,oh,DXGI_FORMAT_R8G8B8A8_UNORM,nullptr,0,D3D11_BIND_UNORDERED_ACCESS);HR(device->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&uavs[i]));}
        DapaNativePlayerMotion state;DapaNativePlayerMotion::Camera camera{};Identity(camera.current);Identity(camera.previous);Identity(camera.jittered);
        D3D11_BOX region{0,0,0,dw,dh,1};
        auto frame=[&](unsigned n){
            state.BeginPair();
            for(int eye=0;eye<2;++eye){state.Capture(eye,ctx.Get(),mv.Get(),&region,dw,dh,views[1].Get(),views[2].Get(),ow,oh,false);state.Observe(eye,camera,n,1000000000LL+n*20000000LL);}
            for(int eye=0;eye<2;++eye)state.Apply(eye,ctx.Get(),1010000000LL+n*20000000LL,views[0].Get(),views[1].Get(),views[2].Get(),uavs[eye].Get());
        };
        frame(1);frame(2);ctx->Flush();
        ComPtr<ID3D11Query> q,begin,end;D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&desc,&q));desc.Query=D3D11_QUERY_TIMESTAMP;HR(device->CreateQuery(&desc,&begin));HR(device->CreateQuery(&desc,&end));
        ctx->Begin(q.Get());ctx->End(begin.Get());for(unsigned n=3;n<13;++n)frame(n);ctx->End(end.Get());ctx->End(q.Get());ctx->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};UINT64 a=0,b=0;
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(ctx->GetData(q.Get(),&frequency,sizeof(frequency),0)==S_FALSE){Check(std::chrono::steady_clock::now()<until,"benchmark GPU timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        HR(ctx->GetData(begin.Get(),&a,sizeof(a),0));HR(ctx->GetData(end.Get(),&b,sizeof(b),0));
        Check(!frequency.Disjoint&&frequency.Frequency&&b>=a,"benchmark timestamps invalid");
        std::printf("Motion coverage: %s\n",sparse?"3.125% of image / 12.5% of mask":"25% of image / entire mask");
        std::printf("LOCAL GPU benchmark: 3072x3264 per-eye output, 2048x2176 guide, 25%% mask, RG16F vectors, 10 stereo pairs: %.3f ms/pair additional GPU elapsed (budgetOff=%d; not in-game FPS)\n",double(b-a)*1000/frequency.Frequency/10,state.BudgetExceeded());
    }
    void Timing() {
        DapaCharacterTiming timing;unsigned reports=0;
        auto report=[&](const std::array<double,4>& values){++reports;for(auto v:values)Check(std::isfinite(v)&&v>=0,"invalid coherent timing");};
        auto drain=[&](){
            ComPtr<ID3D11Query> done;D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT,0};HR(device->CreateQuery(&desc,&done));
            ctx->End(done.Get());ctx->Flush();
            auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while(ctx->GetData(done.Get(),nullptr,0,0)==S_FALSE){Check(std::chrono::steady_clock::now()<until,"timing drain timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        };
        timing.BeginPair();for(unsigned stage=0;stage<2;++stage){auto scope=timing.Measure(ctx.Get(),stage,true);}
        timing.BeginPair();for(unsigned stage=2;stage<4;++stage){auto scope=timing.Measure(ctx.Get(),stage,true);}
        drain();timing.Poll(ctx.Get(),report);Check(reports==0,"mixed-frame timing was admitted");
        timing.Reset();timing.BeginPair();for(unsigned stage=0;stage<4;++stage){auto scope=timing.Measure(ctx.Get(),stage,true);}
        drain();timing.Poll(ctx.Get(),report);Check(reports==1,"complete same-pair timing was lost");
        timing.Poll(ctx.Get(),report);Check(reports==1,"timing sample counted twice");
    }
    void History() {
        DapaNativePlayerMotion state;DapaNativePlayerMotion::Camera c{};Identity(c.jittered);Identity(c.current);Identity(c.previous);
        std::vector<Pixel> v(128*32);auto mv=Texture(128,32,DXGI_FORMAT_R32G32B32A32_FLOAT,v.data(),128*16);
        auto pair=[&](unsigned frame,std::int64_t time,bool badRight=false){
            state.BeginPair();
            for(int eye=0;eye<2;++eye){D3D11_BOX box{UINT(eye*64),0,0,UINT((eye+1)*64),32,1};Check(state.Copy(eye,ctx.Get(),mv.Get(),&box,64,32),"vector copy failed");state.Observe(eye,c,frame+(eye&&badRight?1:0),time);}
        };
        pair(1,1000000000);Check(!state.Ready(1010000000),"first frame must not predict");
        pair(2,1020000000);Check(state.Ready(1030000000),"adjacent stereo pair rejected");
        Check(!state.Ready(1100000000),"stale prediction accepted");
        pair(3,1040000000,true);Check(!state.Ready(1050000000),"mixed stereo generation accepted");
        state.Reset();pair(10,2000000000);pair(12,2040000000);Check(!state.Ready(2050000000),"skipped history accepted");
        state.Reset();pair(20,3000000000);c.previous[12]=.2f;pair(21,3020000000);Check(!state.Ready(3030000000),"camera history mismatch accepted");
        Fixture replacement(D3D_DRIVER_TYPE_WARP);
        auto nextMV=replacement.Texture(128,32,DXGI_FORMAT_R32G32B32A32_FLOAT,v.data(),128*16);
        D3D11_BOX box{0,0,0,64,32,1};
        Check(state.Upload(0,ctx.Get(),3030000000,false)!=nullptr,"first device constants");
        Check(state.Copy(0,replacement.ctx.Get(),nextMV.Get(),&box,64,32),"device replacement copy");
        ComPtr<ID3D11Resource> copied;state.View(0)->GetResource(&copied);
        ComPtr<ID3D11Device> owner;copied->GetDevice(&owner);
        Check(owner==replacement.device,"old device texture retained");
        auto* cb=state.Upload(0,replacement.ctx.Get(),3030000000,false);
        Check(cb!=nullptr,"replacement constants failed");cb->GetDevice(&owner);
        Check(owner==replacement.device,"old device constants retained");
        state.Observe(0,c,22,3040000000);
        Check(!state.Ready(3050000000),"device replacement retained ready stereo history");
        state.Shutdown();Check(!state.Ready(3030000000),"shutdown retained history");
    }
};
static void Equal(const Pixel& a,const Pixel& b,const char* message){for(int i=0;i<4;++i)Check(std::abs(a[i]-b[i])<.0001f,message);}
static void Test(D3D_DRIVER_TYPE driver) {
    Fixture f(driver);DapaNativePlayerMotion::Constants c{};Identity(c.clipToPrevious);Identity(c.clipToCurrent);c.fraction=.5f;
    auto baseline=f.Run(c,0,0);c.enabled=1;
    auto stationary=f.Run(c,0,0);
    for(size_t i=0;i<baseline.size();++i)Equal(baseline[i],stationary[i],"stationary image changed");
    for(int div:{1,2})for(bool flip:{false,true}) {
        auto flat=f.Run(c,0,0,flip,div);auto moved=f.Run(c,-4.f/64,0,flip,div);
        auto expected=flat[16*64+32];expected[0]-=2.f/64;
        Equal(moved[16*64+32],expected,"animated body motion sign/scale failed");
        for(unsigned y=8;y<24;++y)for(unsigned x=24;x<40;++x) {
            auto interior=flat[y*64+x];interior[0]-=2.f/64;
            Equal(moved[y*64+x],interior,"parallel tile quadrant coverage failed");
        }
        Equal(moved[16*64+8],flat[16*64+8],"world changed");
        Equal(moved[16*64+16],flat[16*64+16],"unsupported silhouette changed");
        auto vertical=f.Run(c,0,-4.f/32,flip,div);expected=flat[16*64+32];expected[1]-=2.f/32;
        Equal(vertical[16*64+32],expected,"vertical/flip motion failed");
    }
    c.clipToPrevious[12]=-8.f/64;
    auto camera=f.Run(c,-4.f/64,0);
    for(size_t i=0;i<baseline.size();++i)Equal(camera[i],baseline[i],"camera motion applied twice");
    auto mixed=f.Run(c,-8.f/64,0);auto expected=baseline[16*64+32];expected[0]-=2.f/64;
    Equal(mixed[16*64+32],expected,"camera residual subtraction failed");
    auto cleared=f.Run(c,0,0);Equal(cleared[16*64+32],baseline[16*64+32],"missing vectors inferred as motion");
    Identity(c.clipToPrevious);c.clipToCurrent[12]=.02f;c.clipToPrevious[12]=.02f;
    auto jitter=f.Run(c,-4.f/64,0);Equal(jitter[16*64+32],expected,"matched jitter leaked into motion");
    auto nan=f.Run(c,std::numeric_limits<float>::quiet_NaN(),0);Equal(nan[16*64+32],baseline[16*64+32],"invalid vectors used");
    auto large=f.Run(c,-.5f,0);Equal(large[16*64+32],baseline[16*64+32],"large vectors used");
    auto discontinuous=f.Run(c,-4.f/64,0,false,1,false,0,DXGI_FORMAT_R32G32B32A32_FLOAT,true);
    Equal(discontinuous[16*64+32],baseline[16*64+32],"inconsistent motion correspondence was accepted");
    auto occluded=f.Run(c,-4.f/64,0,false,1,true);Equal(occluded[16*64+32],baseline[16*64+32],"occluded player moved");
    Identity(c.clipToCurrent);Identity(c.clipToPrevious);
    for(float strafe:{-.15f,-.02f,.02f,.15f})for(int div:{1,2})for(bool flip:{false,true}) {
        c.enabled=0;auto original=f.Run(c,0,0,flip,div,false,strafe);
        c.enabled=1;auto corrected=f.Run(c,-4.f/64,0,flip,div,false,strafe);
        unsigned changed=0;
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x) {
            const unsigned rawY=flip?31-y:y;
            bool player=x>=16 && x<48 && rawY>=2 && rawY<30;
            if(!player)Check(memcmp(&original[y*64+x],&corrected[y*64+x],sizeof(Pixel))==0,"WORLD PIXEL CHANGED by character pass");
            else if(memcmp(&original[y*64+x],&corrected[y*64+x],sizeof(Pixel))!=0)++changed;
        }
        Check(changed>0,"character pass did not change masked interior");
    }
    for(auto format:{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        c.enabled=1;auto moved=f.Run(c,-4.f/64,0,false,2,false,0,format);
        auto expected=baseline[16*64+32];expected[0]-=2.f/64;
        Equal(moved[16*64+32],expected,"native vector format motion failed");
    }
    f.History();f.Timing();
    if(driver==D3D_DRIVER_TYPE_HARDWARE){f.Benchmark();f.Benchmark(true);}
    std::printf("PASS %s: native-vector motion, camera cancellation, scaled guides, vertical flip, masked world/edges, invalid data, stereo history, state restoration\n",driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware");
}
int main(int argc,char** argv){try{
    if(argc>1 && std::strcmp(argv[1],"--benchmark-only")==0){Fixture f(D3D_DRIVER_TYPE_HARDWARE);f.Benchmark();f.Benchmark(true);return 0;}
    DapaCharacterBudget budget;
    budget.Observe(2.0);budget.Observe(.3);Check(!budget.disabled&&budget.consecutiveOver==0,"transient budget spike latched off");
    budget.Observe(1.6);budget.Observe(1.7);Check(!budget.disabled,"budget disabled before three samples");
    budget.Observe(1.8);Check(budget.disabled&&budget.tripMs==1.8,"sustained budget excess ignored");
    budget.Observe(.1);Check(budget.tripMs==1.8,"trip measurement overwritten");
    Check(!budget.TryRecover(1999),"budget resumed before cooldown");
    Check(budget.TryRecover(2000)&&!budget.disabled&&budget.recoveryAttempts==1,"budget recovery missing");
    budget.Observe(2,2000);budget.Observe(2,2000);budget.Observe(2,2000);
    Check(budget.RemainingMs(2000)==4000,"repeated failure did not back off");
    Check(!budget.TryRecover(5999)&&budget.TryRecover(6000),"second cooldown boundary");
    for(unsigned i=0;i<10;++i)budget.Observe(.3,6000+i);
    budget.Observe(2,7000);budget.Observe(2,7000);budget.Observe(2,7000);
    Check(budget.RemainingMs(7000)==2000,"stable recovery did not reset backoff");
    std::uint64_t now=9000;
    for(unsigned n=0;n<8;++n){
        Check(budget.TryRecover(now),"backoff retry missing");
        budget.Observe(2,now);budget.Observe(2,now);budget.Observe(2,now);
        Check(budget.RemainingMs(now)<=30000,"backoff exceeded cap");
        now+=budget.RemainingMs(now);
    }
    Check(budget.RemainingMs(now-1)==1,"capped cooldown not deterministic");
    DapaNativePlayerMotion::Camera c{};Identity(c.jittered);Identity(c.current);Identity(c.previous);
    c.position[0]=2;c.previousPosition[0]=1;DapaNativePlayerMotion::Constants data{};
    Check(DapaNativePlayerMotion::CameraTransforms(c,data)&&std::abs(data.clipToPrevious[12]-1)<.0001f,"camera-relative origin not reconciled");
    c.position[0]=100;Check(!DapaNativePlayerMotion::CameraTransforms(c,data),"camera jump admitted");
    Check(std::abs(DapaNativePlayerMotion::Fraction(1020000000,1000000000,1030000000)-.5f)<.0001f,"prediction fraction");
    Check(DapaNativePlayerMotion::Fraction(1020000000,1000000000,1010000000)==0,"backward prediction");
    Test(D3D_DRIVER_TYPE_WARP);
    if(argc<2 || std::strcmp(argv[1],"--warp-only")!=0)Test(D3D_DRIVER_TYPE_HARDWARE);
    std::printf("PASS %u checks\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}}
