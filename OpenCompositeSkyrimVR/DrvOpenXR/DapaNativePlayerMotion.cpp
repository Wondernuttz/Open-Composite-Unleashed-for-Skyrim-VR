#include "DapaNativePlayerMotion.h"
#include "DapaComputeState.h"
#include "DapaCharacterComputeState.h"
#include "DapaCharacterShader.h"
#include <d3dcompiler.h>
#include <cstring>

namespace {
void Multiply(const float* a,const float* b,float* result) {
    for(int r=0;r<4;++r)for(int c=0;c<4;++c) {
        double v=0;for(int k=0;k<4;++k)v+=double(a[r*4+k])*b[k*4+c];
        result[r*4+c]=float(v);
    }
}
bool Close(const float* a,const float* b,int count) {
    for(int i=0;i<count;++i)
        if(!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::abs(a[i]-b[i])>0.0001f*(std::max)(1.f,(std::max)(std::abs(a[i]),std::abs(b[i]))))return false;
    return true;
}
}
void DapaNativePlayerMotion::Reset() {
    for(auto& eye:eyes) {
        eye.lastFrame=eye.frame=0;eye.lastTime=eye.time=eye.previousTime=0;
        eye.copied=eye.ready=eye.tilesReady=eye.residualReady=false;eye.data={};eye.last={};
    }
}
void DapaNativePlayerMotion::Shutdown() { eyes[0]={};eyes[1]={};constants.Reset();tileShader.Reset();characterShader.Reset();residualShader.Reset();sampler.Reset();pipelineDevice.Reset();timing.Reset();budget={};for(auto& cost:costs)cost=0; }
void DapaNativePlayerMotion::BeginPair() {
    timing.BeginPair();
    for(auto& eye:eyes){eye.copied=eye.ready=eye.tilesReady=eye.residualReady=false;eye.frame=0;eye.time=0;}
}
bool DapaNativePlayerMotion::Copy(int index,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,const D3D11_BOX* box,unsigned width,unsigned height) {
    if(index<0||index>1||!ctx||!source||!box)return false;
    auto& eye=eyes[index];eye.copied=eye.ready=eye.tilesReady=eye.residualReady=false;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    if(!width||!height||box->front!=0||box->back!=1||box->right<=box->left||box->bottom<=box->top||
       box->right>desc.Width||box->bottom>desc.Height||box->right-box->left!=width||box->bottom-box->top!=height||
       desc.ArraySize!=1||desc.SampleDesc.Count!=1)return false;
    if(desc.Format!=DXGI_FORMAT_R16G16_FLOAT && desc.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT &&
       desc.Format!=DXGI_FORMAT_R32G32_FLOAT && desc.Format!=DXGI_FORMAT_R32G32B32A32_FLOAT)return false;
    Ptr<ID3D11Device> device,owner;ctx->GetDevice(&device);source->GetDevice(&owner);
    if(device!=owner)return false;
    Ptr<ID3D11Device> cachedOwner;
    if(eye.texture)eye.texture->GetDevice(&cachedOwner);
    if(cachedOwner && cachedOwner!=device) {
        eye={}; // Never copy across devices or retain history from another device.
    }
    if(eye.source!=source||eye.width!=width||eye.height!=height||eye.format!=desc.Format) {
        eye.lastFrame=0;eye.lastTime=0;eye.source=source;
    }
    if(!eye.srv||eye.width!=width||eye.height!=height||eye.format!=desc.Format) {
        eye.srv.Reset();eye.texture.Reset();eye.residual.Reset();eye.residualSrv.Reset();eye.residualUav.Reset();
        desc.Width=width;desc.Height=height;desc.MipLevels=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.CPUAccessFlags=desc.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&eye.texture))||
           FAILED(device->CreateShaderResourceView(eye.texture.Get(),nullptr,&eye.srv)))return false;
        eye.width=width;eye.height=height;eye.format=desc.Format;
    }
    DapaPredicationState unpredicated(ctx);
    ctx->CopySubresourceRegion(eye.texture.Get(),0,0,0,0,source,0,box);
    eye.copied=true;return true;
}
bool DapaNativePlayerMotion::CameraTransforms(const Camera& camera,Constants& out) {
    float inverse[16],shift[16]{},prior[16];
    if(!DapaMotion::Inverse(camera.jittered,inverse))return false;
    for(int i=0;i<16;++i)if(!std::isfinite(camera.current[i])||!std::isfinite(camera.previous[i]))return false;
    for(int i=0;i<4;++i)shift[i*5]=1;
    for(int i=0;i<3;++i) {
        const float delta=camera.position[i]-camera.previousPosition[i];
        if(!std::isfinite(delta)||std::abs(delta)>20.f)return false;
        shift[12+i]=delta;
    }
    Multiply(inverse,shift,prior);
    Multiply(prior,camera.previous,out.clipToPrevious);
    Multiply(inverse,camera.current,out.clipToCurrent);
    for(int i=0;i<16;++i)if(!std::isfinite(out.clipToPrevious[i])||!std::isfinite(out.clipToCurrent[i]))return false;
    return true;
}
float DapaNativePlayerMotion::Fraction(std::int64_t current,std::int64_t previous,std::int64_t target) {
    const double dt=double(current-previous)*1e-9, ahead=double(target-current)*1e-9;
    if(previous<=0||dt<.004||dt>.100||ahead<=0||ahead>.030||ahead>dt)return 0;
    return float(ahead/dt);
}
void DapaNativePlayerMotion::Observe(int index,const Camera& camera,std::uint32_t frame,std::int64_t time) {
    if(index<0||index>1)return;
    auto& eye=eyes[index];eye.ready=false;eye.frame=frame;eye.time=time;eye.previousTime=eye.lastTime;
    Constants data{};
    const bool cameraValid=CameraTransforms(camera,data);
    eye.ready=eye.copied && frame && eye.lastFrame && frame-eye.lastFrame==1 &&
        time>eye.lastTime && cameraValid && Close(camera.previous,eye.last.current,16) &&
        Close(camera.previousPosition,eye.last.position,3);
    data.vectorSize[0]=float(eye.width);data.vectorSize[1]=float(eye.height);eye.data=data;
    eye.last=camera;eye.lastFrame=cameraValid?frame:0;eye.lastTime=cameraValid?time:0;
}
bool DapaNativePlayerMotion::Ready(std::int64_t target) const {
    return eyes[0].ready&&eyes[1].ready&&eyes[0].frame==eyes[1].frame&&
        eyes[0].time==eyes[1].time&&eyes[0].previousTime==eyes[1].previousTime&&
        eyes[0].width==eyes[1].width&&eyes[0].height==eyes[1].height&&eyes[0].format==eyes[1].format&&
        Fraction(eyes[0].time,eyes[0].previousTime,target)>0;
}
ID3D11Buffer* DapaNativePlayerMotion::Upload(int index,ID3D11DeviceContext* ctx,std::int64_t target,bool allowed) {
    if(index<0||index>1||!ctx)return nullptr;
    Ptr<ID3D11Device> activeDevice,bufferDevice;ctx->GetDevice(&activeDevice);
    if(constants)constants->GetDevice(&bufferDevice);
    if(bufferDevice && bufferDevice!=activeDevice)constants.Reset();
    if(!constants) {
        Ptr<ID3D11Device> device;ctx->GetDevice(&device);
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof(Constants);desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(FAILED(device->CreateBuffer(&desc,nullptr,&constants)))return nullptr;
    }
    auto data=eyes[index].data;
    data.enabled=allowed&&Ready(target)?1.f:0.f;
    data.fraction=data.enabled?Fraction(eyes[index].time,eyes[index].previousTime,target):0;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(FAILED(ctx->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped)))return nullptr;
    std::memcpy(mapped.pData,&data,sizeof(data));ctx->Unmap(constants.Get(),0);
    return constants.Get();
}

bool DapaNativePlayerMotion::Pipeline(ID3D11DeviceContext* ctx) {
    Ptr<ID3D11Device> device;ctx->GetDevice(&device);
    if(pipelineDevice && pipelineDevice!=device)Shutdown();
    if(tileShader && characterShader && residualShader && sampler)return true;
    pipelineDevice=device;
    auto compile=[&](const char* entry,Ptr<ID3D11ComputeShader>& shader) {
        Ptr<ID3DBlob> code,errors;
        HRESULT hr=D3DCompile(s_characterShaderHLSL,strlen(s_characterShaderHLSL),nullptr,nullptr,nullptr,
            entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        return SUCCEEDED(hr) && SUCCEEDED(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
    };
    if(!residualShader && !compile("PrepareResidual",residualShader))return false;
    if(!tileShader && !compile("BuildTiles",tileShader))return false;
    if(!characterShader && !compile("CorrectCharacter",characterShader))return false;
    if(!sampler) {
        D3D11_SAMPLER_DESC desc{};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;desc.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(device->CreateSamplerState(&desc,&sampler)))return false;
    }
    return true;
}
bool DapaNativePlayerMotion::Prepare(int index,ID3D11DeviceContext* ctx,ID3D11ShaderResourceView* mask,
    ID3D11ShaderResourceView* depth,unsigned width,unsigned height,bool flip) {
    auto& eye=eyes[index];eye.tilesReady=false;
    if(!mask||!depth||!width||!height||width>16384||height>16384||!Pipeline(ctx))return false;
    const UINT count=((width+31)/32)*((height+31)/32);
    if(count>65535)return false; // DispatchIndirect X limit; fail to original DAPA.
    struct TileConstants {float output[2],depth[2],flip[2],reserved[2];};
    const TileConstants values={{float(width),float(height)},{float(eye.width),float(eye.height)},{0,flip?1.f:0.f},{0,0}};
    if(!eye.tiles || eye.outWidth!=width || eye.outHeight!=height) {
        eye.tiles.Reset();eye.tileUav.Reset();eye.tileSrv.Reset();eye.args.Reset();eye.tileConstants.Reset();
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=count*8;desc.StructureByteStride=8;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        if(FAILED(pipelineDevice->CreateBuffer(&desc,nullptr,&eye.tiles)))return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC u{};u.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;u.Buffer.NumElements=count;u.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_APPEND;
        if(FAILED(pipelineDevice->CreateUnorderedAccessView(eye.tiles.Get(),&u,&eye.tileUav)))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.NumElements=count;
        if(FAILED(pipelineDevice->CreateShaderResourceView(eye.tiles.Get(),&v,&eye.tileSrv)))return false;
        const UINT dispatch[3]={0,16,1};D3D11_SUBRESOURCE_DATA initial{dispatch,0,0};
        desc={};desc.ByteWidth=12;desc.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        if(FAILED(pipelineDevice->CreateBuffer(&desc,&initial,&eye.args)))return false;
        desc={};desc.ByteWidth=sizeof(TileConstants);desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        initial={&values,0,0};if(FAILED(pipelineDevice->CreateBuffer(&desc,&initial,&eye.tileConstants)))return false;
        eye.outWidth=width;eye.outHeight=height;eye.flipped=flip;
    }
    if(!eye.tileSrv||!eye.tileUav||!eye.args||!eye.tileConstants)return false;
    DapaCharacterComputeState saved(ctx,false);
    // Source depth dimensions can change independently of the output size.
    ctx->UpdateSubresource(eye.tileConstants.Get(),0,nullptr,&values,0,0);
    eye.tilesReady=true;return true;
}
bool DapaNativePlayerMotion::Capture(int index,ID3D11DeviceContext* ctx,ID3D11Texture2D* source,const D3D11_BOX* box,
    unsigned width,unsigned height,ID3D11ShaderResourceView* mask,ID3D11ShaderResourceView* depth,
    unsigned outputWidth,unsigned outputHeight,bool flip) {
    if(index<0||index>1||!ctx)return false;
    if(index==0 && budget.TryRecover(DapaCharacterTiming::NowMs())) {
        // Discard experimental history only. The next complete real pairs must
        // establish fresh, matching camera/vector history before correction.
        timing.Reset();eyes[0]={};eyes[1]={};constants.Reset();
        for(auto& cost:costs)cost=0;
    }
    if(index==0)timing.Poll(ctx,[&](const std::array<double,4>& sample){
        if(!budget.disabled) {
            for(unsigned n=0;n<4;++n)costs[n]=sample[n];
            budget.Observe(CostMs(),DapaCharacterTiming::NowMs());
        }
    });
    if(budget.disabled) {
        // Retire only this experiment's private buffers when the budget trips.
        eyes[0]={};eyes[1]={};constants.Reset();
        return false;
    }
    if(!Pipeline(ctx))return false;
    const auto& prior=eyes[index];
    const bool warm=prior.texture && prior.args && prior.tileConstants && prior.width==width && prior.height==height &&
        prior.outWidth==outputWidth && prior.outHeight==outputHeight;
    auto measure=timing.Measure(ctx,unsigned(index),warm);
    return Copy(index,ctx,source,box,width,height) && Prepare(index,ctx,mask,depth,outputWidth,outputHeight,flip);
}
bool DapaNativePlayerMotion::Apply(int index,ID3D11DeviceContext* ctx,std::int64_t target,
    ID3D11ShaderResourceView* color,ID3D11ShaderResourceView* mask,ID3D11ShaderResourceView* depth,
    ID3D11UnorderedAccessView* output) {
    if(index<0||index>1||!ctx||!color||!mask||!depth||!output||budget.disabled||!Ready(target)||
       !eyes[0].tilesReady||!eyes[1].tilesReady)return false;
    auto* cb=Upload(index,ctx,target,true);if(!cb)return false;
    auto& eye=eyes[index];
    DapaCharacterComputeState saved(ctx,false);
    auto measure=timing.Measure(ctx,unsigned(index+2),bool(eye.residualSrv));
    // Prepare once per cached real frame, at guide resolution. Include this
    // dispatch in correction timing; output lookups do not repeat camera math.
    if(!eye.residualReady) {
        if(!eye.residualUav) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=eye.width;desc.Height=eye.height;
            desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R16G16_FLOAT;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
            if(FAILED(pipelineDevice->CreateTexture2D(&desc,nullptr,&eye.residual)) ||
               FAILED(pipelineDevice->CreateShaderResourceView(eye.residual.Get(),nullptr,&eye.residualSrv)) ||
               FAILED(pipelineDevice->CreateUnorderedAccessView(eye.residual.Get(),nullptr,&eye.residualUav)))return false;
        }
        ID3D11ShaderResourceView* prepareViews[5]={nullptr,mask,depth,eye.srv.Get(),nullptr};
        ctx->CSSetShaderResources(0,5,prepareViews);
        ID3D11UnorderedAccessView* prepareUavs[3]={nullptr,nullptr,eye.residualUav.Get()};
        ctx->CSSetUnorderedAccessViews(0,3,prepareUavs,nullptr);
        ID3D11Buffer* prepareBuffers[2]={cb,eye.tileConstants.Get()};ctx->CSSetConstantBuffers(0,2,prepareBuffers);
        ctx->CSSetShader(residualShader.Get(),nullptr,0);ctx->Dispatch((eye.width+7)/8,(eye.height+7)/8,1);
        ID3D11UnorderedAccessView* noUavs[3]{};ctx->CSSetUnorderedAccessViews(0,3,noUavs,nullptr);
        // Admit only output tiles containing usable non-negligible motion.
        // This scan uses the prepared RG16 map instead of re-reading two depths.
        ID3D11ShaderResourceView* tileViews[5]={nullptr,nullptr,nullptr,eye.residualSrv.Get(),nullptr};
        ctx->CSSetShaderResources(0,5,tileViews);
        ID3D11UnorderedAccessView* tileUavs[3]={nullptr,eye.tileUav.Get(),nullptr};
        const UINT counts[3]={UINT(-1),0,UINT(-1)};ctx->CSSetUnorderedAccessViews(0,3,tileUavs,counts);
        ctx->CSSetShader(tileShader.Get(),nullptr,0);
        ctx->Dispatch((eye.outWidth+31)/32,(eye.outHeight+31)/32,1);
        ctx->CSSetUnorderedAccessViews(0,3,noUavs,nullptr);
        ctx->CopyStructureCount(eye.args.Get(),0,eye.tileUav.Get());
        eye.residualReady=true;
    }
    ID3D11ShaderResourceView* views[5]={color,mask,depth,eye.residualSrv.Get(),eye.tileSrv.Get()};
    ctx->CSSetShaderResources(0,5,views);
    ID3D11UnorderedAccessView* uavs[2]={output,nullptr};ctx->CSSetUnorderedAccessViews(0,2,uavs,nullptr);
    ID3D11Buffer* buffers[2]={cb,eye.tileConstants.Get()};ctx->CSSetConstantBuffers(0,2,buffers);
    ctx->CSSetSamplers(0,1,sampler.GetAddressOf());ctx->CSSetShader(characterShader.Get(),nullptr,0);
    ctx->DispatchIndirect(eye.args.Get(),0);
    return true;
}
