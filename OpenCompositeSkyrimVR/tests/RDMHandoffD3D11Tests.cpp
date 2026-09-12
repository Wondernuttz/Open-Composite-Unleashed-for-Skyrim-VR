#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cstring>
#include "OpenOVR/Compositor/RDMRenderScope.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/ExactPixelShader.h"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* msg) { std::puts(msg); }
void oovr_log_raw_format(const char*, long, const char*, const char* fmt, ...) {
    va_list args; va_start(args,fmt); std::vprintf(fmt,args); va_end(args); std::puts("");
}
static void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) { if(FAILED(hr)) { std::printf("HRESULT=%08X\n",unsigned(hr)); throw std::runtime_error("D3D11 call failed"); } }
static ComPtr<ID3DBlob> Compile(const char* source,const char* entry,const char* target) {
    ComPtr<ID3DBlob> blob,errors;
    HRESULT hr=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors);
    if(FAILED(hr) && errors) std::puts(static_cast<const char*>(errors->GetBufferPointer())); HR(hr);return blob;
}
static std::vector<unsigned char> Read(ID3D11Device* dev,ID3D11DeviceContext* ctx,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING;
    d.BindFlags=0;d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging; HR(dev->CreateTexture2D(&d,nullptr,&staging));
    ctx->CopyResource(staging.Get(),texture);
    D3D11_MAPPED_SUBRESOURCE m{};HR(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&m));
    UINT stride=d.Format==DXGI_FORMAT_R16_UNORM?2:d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    std::vector<unsigned char> bytes(d.Width*d.Height*stride);
    for(UINT y=0;y<d.Height;++y) std::memcpy(bytes.data()+y*d.Width*stride,static_cast<char*>(m.pData)+y*m.RowPitch,d.Width*stride);
    ctx->Unmap(staging.Get(),0);return bytes;
}

// Model CSX's vtable wrapper: native draw first, then accepted-draw callback.
// The callback must never see OCU's private DSV. Target wrappers are the same
// notification contract used by dx11compositor's existing scoping hooks.
namespace Wrapped {
using Draw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
using Targets=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView*const*,ID3D11DepthStencilView*);
using TargetsUav=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,ID3D11RenderTargetView*const*,ID3D11DepthStencilView*,UINT,UINT,ID3D11UnorderedAccessView*const*,const UINT*);
Draw draw;Targets targets;TargetsUav targetsUav;
ID3D11DepthStencilView* expected=nullptr;
ID3D11VertexShader* expectedShader=nullptr;
bool check=false; unsigned accepted=0;
bool checkDepthGuide=false; unsigned acceptedDepth=0;
void STDMETHODCALLTYPE DrawCall(ID3D11DeviceContext* c,UINT n,UINT first) {
    draw(c,n,first);
    if(checkDepthGuide) {
        ComPtr<ID3D11DepthStencilView> d;c->OMGetRenderTargets(0,nullptr,&d);
        ComPtr<ID3D11PixelShader> p;c->PSGetShader(&p,nullptr,nullptr);
        Require(d.Get()==expected && !p,"depth-prepass observer saw OCU guide bindings");++acceptedDepth;
    }
    ComPtr<ID3D11VertexShader> bound;c->VSGetShader(&bound,nullptr,nullptr);
    if(check && bound.Get()==expectedShader) { ComPtr<ID3D11DepthStencilView> d;c->OMGetRenderTargets(0,nullptr,&d);
        Require(d.Get()==expected,"CSX post-draw observer saw private depth");++accepted; }
}
void STDMETHODCALLTYPE SetTargets(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d) {
    targets(c,n,rt,d);RDMRenderScope::NotifyTargets(c);
}
void STDMETHODCALLTYPE SetTargetsUav(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d,UINT start,UINT count,ID3D11UnorderedAccessView*const* u,const UINT* initial) {
    targetsUav(c,n,rt,d,start,count,u,initial);RDMRenderScope::NotifyTargets(c);
}
struct Table {
    ID3D11DeviceContext* context; void** original;std::array<void*,134> table;
    Table(ID3D11DeviceContext* c):context(c),original(*reinterpret_cast<void***>(c)) {
        std::copy_n(original,table.size(),table.begin());
        draw=reinterpret_cast<Draw>(table[13]);targets=reinterpret_cast<Targets>(table[33]);targetsUav=reinterpret_cast<TargetsUav>(table[34]);
        table[13]=reinterpret_cast<void*>(&DrawCall);table[33]=reinterpret_cast<void*>(&SetTargets);table[34]=reinterpret_cast<void*>(&SetTargetsUav);
        *reinterpret_cast<void***>(c)=table.data();
    }
    ~Table() { check=false; *reinterpret_cast<void***>(context)=original; }
};
}

static constexpr char shader[] = R"HLSL(
cbuffer Params:register(b0) { float z; float width; float2 pad; };
float4 VS(uint id:SV_VertexID):SV_POSITION {
    return float4(id==2?3:-1,id==1?-3:1,z,1);
}
struct Outputs { float4 color:SV_Target0;float4 motion:SV_Target1; };
Outputs PS(float4 p:SV_POSITION) {
    Outputs o; o.color=p.x<width*0.5?float4(0.8,0.1,0.2,1):float4(0.1,0.7,0.2,1);
    o.motion=float4(0.25,0.5,0.75,1);return o;
}
Outputs Alpha(float4 p:SV_POSITION) { if((uint(p.x)&1)==0) discard;return PS(p); }
float4 One(float4 p:SV_POSITION):SV_Target0 { return PS(p).color; }
float4 MeshVS(uint id:SV_VertexID):SV_POSITION {
    const float2 uv[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float x=lerp(pad.x,pad.y,uv[id].x);
    return float4(2*x/width-1,1-2*uv[id].y,z,1);
}
Outputs MeshPS(float4 p:SV_POSITION) {
    Outputs o;
    o.color=pad.y-pad.x<3?float4(.9,.1,.7,1):float4(.2,.8,.3,1);
    o.motion=pad.y-pad.x<3?float4(.75,.25,.5,1):float4(.25,.5,.75,1);
    return o;
}
)HLSL";
static void Run(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver,UINT width,UINT height,bool d24,
    DXGI_FORMAT auxiliaryFormat=DXGI_FORMAT_R8G8B8A8_UNORM,bool separateSubmittedEyes=false) {
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    UINT flags=D3D11_CREATE_DEVICE_DEBUG;
    HRESULT created=D3D11CreateDevice(adapter,driver,nullptr,flags,levels,2,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(created==DXGI_ERROR_SDK_COMPONENT_MISSING) { flags=0;created=D3D11CreateDevice(adapter,driver,nullptr,flags,levels,2,D3D11_SDK_VERSION,&dev,&level,&ctx); }
    HR(created);
    ComPtr<ID3D11InfoQueue> info;dev.As(&info);
    Require(ocu_vrs_guard::InstallShaderCapture(dev.Get()),"shader capture failed");
    auto vsCode=Compile(shader,"VS","vs_5_0"),psCode=Compile(shader,"PS","ps_5_0"),alphaCode=Compile(shader,"Alpha","ps_5_0"),oneCode=Compile(shader,"One","ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps,alpha,one;
    HR(dev->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
    HR(dev->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
    HR(dev->CreatePixelShader(alphaCode->GetBufferPointer(),alphaCode->GetBufferSize(),nullptr,&alpha));
    HR(dev->CreatePixelShader(oneCode->GetBufferPointer(),oneCode->GetBufferSize(),nullptr,&one));
    Require(ocu_vrs_guard::ShaderColorOutputs(ps.Get())==3,"MRT shader output classification failed");
    ComPtr<ID3DBlob> stripped;HR(D3DStripShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),
        D3DCOMPILER_STRIP_REFLECTION_DATA|D3DCOMPILER_STRIP_DEBUG_INFO,&stripped));
    ComPtr<ID3D11PixelShader> strippedPS;HR(dev->CreatePixelShader(stripped->GetBufferPointer(),stripped->GetBufferSize(),nullptr,&strippedPS));
    Require(ocu_vrs_guard::ShaderColorOutputs(strippedPS.Get())==3,"stripped shader lost MRT classification");
    D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> color,motion;ComPtr<ID3D11RenderTargetView> colorRT,motionRT;
    ComPtr<ID3D11ShaderResourceView> colorSRV;
    HR(dev->CreateTexture2D(&td,nullptr,&color));td.Format=auxiliaryFormat;HR(dev->CreateTexture2D(&td,nullptr,&motion));
    HR(dev->CreateRenderTargetView(color.Get(),nullptr,&colorRT));HR(dev->CreateRenderTargetView(motion.Get(),nullptr,&motionRT));
    HR(dev->CreateShaderResourceView(color.Get(),nullptr,&colorSRV));
    td.Format=d24?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> depthView;
    HR(dev->CreateTexture2D(&td,nullptr,&depth));
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=d24?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_D32_FLOAT;
    dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;HR(dev->CreateDepthStencilView(depth.Get(),&dv,&depthView));
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> pre,lit;HR(dev->CreateDepthStencilState(&ds,&pre));
    ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_EQUAL;HR(dev->CreateDepthStencilState(&ds,&lit));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.Usage=D3D11_USAGE_DEFAULT;
    float params[4]={0.4f,float(width),0,0};D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=params;
    ComPtr<ID3D11Buffer> cb;HR(dev->CreateBuffer(&bd,&initial,&cb));auto* constants=cb.Get();
    D3D11_RASTERIZER_DESC noCullDesc{};noCullDesc.FillMode=D3D11_FILL_SOLID;noCullDesc.CullMode=D3D11_CULL_NONE;noCullDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> noCull;HR(dev->CreateRasterizerState(&noCullDesc,&noCull));
    Wrapped::Table wrapped(ctx.Get());
    RDMRenderScope rdm;
    auto ArmSources=[&](ID3D11Texture2D* sceneDepth,ID3D11Texture2D* submitted) {
        float centers[4]={0.45f,0.5f,0.55f,0.5f};
        return rdm.Arm(ctx.Get(),sceneDepth,submitted,width,height,
            {0,0,int(width/2),int(height)},{int(width/2),0,int(width-width/2),int(height)},
            {0.3f,0.6f,true,false,{}},centers);
    };
    auto Arm=[&] {
        Require(ArmSources(depth.Get(),separateSubmittedEyes?nullptr:color.Get()),"RDM arm failed");
    };
    auto Bind=[&](bool equal=true) {
        ID3D11RenderTargetView* targets[]={colorRT.Get(),motionRT.Get()};
        ctx->OMSetRenderTargets(2,targets,depthView.Get());
        ctx->OMSetDepthStencilState(equal?lit.Get():pre.Get(),0);
        ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);ctx->RSSetState(noCull.Get());
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};ctx->RSSetViewports(1,&viewport);
        ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
        ctx->VSSetConstantBuffers(0,1,&constants);ctx->PSSetConstantBuffers(0,1,&constants);
        ctx->IASetInputLayout(nullptr);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    auto Prepare=[&](bool armed=false) {
        rdm.EndFrame();Wrapped::check=false;
        if(armed)Arm();
        ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH|(d24?D3D11_CLEAR_STENCIL:0),1,0xA7);
        Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
        const FLOAT zero[4]{};ctx->ClearRenderTargetView(colorRT.Get(),zero);ctx->ClearRenderTargetView(motionRT.Get(),zero);
        Bind();
    };
    std::vector<unsigned char> expectedColor,expectedMotion;
    auto CheckColor=[&] {
        auto pixels=Read(dev.Get(),ctx.Get(),color.Get());auto mv=Read(dev.Get(),ctx.Get(),motion.Get());
        Require(pixels==expectedColor,"color differs from this GPU's full-rate reference");
        Require(mv==expectedMotion,"auxiliary/MV target differs from full-rate reference");
    };
    Prepare();ctx->Draw(3,0);
    expectedColor=Read(dev.Get(),ctx.Get(),color.Get());expectedMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    if(separateSubmittedEyes) {
        // The submit textures are separate, but the render bridge supplies the
        // shared scene depth. It must be sufficient without widening ownership
        // to unrelated same-size targets, missing bindings, or offscreen passes.
        Prepare(true);
        D3D11_TEXTURE2D_DESC otherDesc{};depth->GetDesc(&otherDesc);
        ComPtr<ID3D11Texture2D> otherDepth;ComPtr<ID3D11DepthStencilView> otherDSV;
        HR(dev->CreateTexture2D(&otherDesc,nullptr,&otherDepth));
        HR(dev->CreateDepthStencilView(otherDepth.Get(),&dv,&otherDSV));
        ID3D11RenderTargetView* targets[]={colorRT.Get(),motionRT.Get()};
        ctx->OMSetRenderTargets(2,targets,otherDSV.Get());
        Require(!rdm.BeforeDraw(),"separate-submit RDM accepted unrelated same-size depth");
        ctx->OMSetRenderTargets(2,targets,nullptr);
        Require(!rdm.BeforeDraw(),"separate-submit RDM accepted missing scene depth binding");
        ctx->OMSetRenderTargets(0,nullptr,depthView.Get());
        Require(!rdm.BeforeDraw(),"separate-submit RDM accepted a depth-only pass");
        Bind();D3D11_VIEWPORT offscreen{0,0,float(width/4),float(height/2),0,1};
        ctx->RSSetViewports(1,&offscreen);
        Require(!rdm.BeforeDraw(),"separate-submit RDM accepted a partial offscreen viewport");
        Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==1,"separate-submit RDM failed to resume on the exact main depth");
        CheckColor();
        // Loss of both ownership sources must disarm, and a stale depth guide
        // must not be reused after the bridge returns without a new prepass.
        Require(!ArmSources(nullptr,nullptr) && !RDMRenderScope::Active(ctx.Get()),
            "RDM accepted missing depth and submitted color");
        Bind();ctx->Draw(3,0);CheckColor();
        Require(ArmSources(nullptr,color.Get()),"existing submitted-color scope failed to arm");
        Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==0,"color-only scope reused a stale depth guide");
        Arm();Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==0,"bridge recovery used depth ownership from before loss");
        Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==1,"separate-submit RDM did not recover after a fresh prepass");
        CheckColor();rdm.EndFrame();
    }
    // The explicit internal-shader request protects even an otherwise eligible
    // full-channel opaque draw; it does not depend on DAPA's RED-only blend state.
    Prepare(true);
    Require(ocu_exact_pixels::Mark(ps.Get()),"exact-pixel request failed");
    ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==0&&ocu_vrs_guard::CurrentReasons(ctx.Get())==ocu_vrs_guard::ExactPixels,
        "RDM ignored explicit exact-pixel request");
    CheckColor();
    HR(ps->SetPrivateData(ocu_exact_pixels::MetadataId,0,nullptr));
    ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==1,"ordinary RDM eligibility did not resume after exact draw");
    rdm.EndFrame();CheckColor();
    Prepare(true);auto expectedDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Bind();
    // Measure the scene draw alone, excluding mask setup and reconstruction.
    Require(rdm.BeforeDraw(),"eligible color pass rejected");
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;HR(dev->CreateQuery(&qd,&query));
    ctx->Begin(query.Get());ctx->Draw(3,0);ctx->End(query.Get());
    const auto sparse=Read(dev.Get(),ctx.Get(),color.Get());
    unsigned holes=0;for(size_t i=3;i<sparse.size();i+=4) holes+=sparse[i]==0;
    rdm.AfterDraw(true);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS pipeline{};HRESULT queryResult=S_FALSE;
    for(unsigned i=0;i<100000 && queryResult==S_FALSE;++i) queryResult=ctx->GetData(query.Get(),&pipeline,sizeof(pipeline),0);
    std::printf("measured scene invocations=%llu full=%u sparse holes=%u\n",pipeline.PSInvocations,width*height,holes);
    if(info && !pipeline.PSInvocations) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> data(size);
        auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());info->GetMessage(i,m,&size);std::puts(m->pDescription);
    }
    HR(queryResult);Require(queryResult==S_OK && pipeline.PSInvocations>0 && holes>0,"RDM did not create sparse scene coverage");
    if(driver!=D3D_DRIVER_TYPE_WARP) Require(pipeline.PSInvocations<width*height,"RDM did not reduce hardware pixel shading");
    // Sampling the MRT forces its resolve before the binding is passed down.
    auto* input=colorSRV.Get();ctx->OMSetRenderTargets(0,nullptr,nullptr);ctx->CSSetShaderResources(0,1,&input);
    Require(rdm.Stats().resolves==2,"shader consumer missed MRT handoff");
    CheckColor();Require(Read(dev.Get(),ctx.Get(),depth.Get())==expectedDepth,"RDM changed game depth/stencil");
    auto counts=rdm.Stats();std::printf("scene PS invocations=%llu full=%u resolves=%u\n",pipeline.PSInvocations,width*height,counts.resolves);
    ID3D11ShaderResourceView* nullSrv=nullptr;ctx->CSSetShaderResources(0,1,&nullSrv);
    for(int boundary=0;boundary<4;++boundary) {
        Prepare(true);Bind();Wrapped::expected=depthView.Get();Wrapped::expectedShader=vs.Get();Wrapped::check=true;
        ctx->Draw(3,0);ctx->Draw(3,0);
        Wrapped::check=false;
        Require(rdm.Stats().maskedDraws==2,"native draw hooks missed wrapped draws");
        Require(rdm.Stats().batches==1,"unchanged draws rebuilt private mask");
        if(boundary==0) { auto result=Read(dev.Get(),ctx.Get(),color.Get()); }
        if(boundary==1) ctx->Dispatch(0,0,0);
        if(boundary==2) { ctx->PSSetShader(alpha.Get(),nullptr,0);ctx->Draw(3,0); }
        if(boundary==3) rdm.EndFrame();
        Require(rdm.Stats().resolves==2,"consumer did not resolve both MRTs");CheckColor();
        Require(Read(dev.Get(),ctx.Get(),depth.Get())==expectedDepth,"downstream depth no longer matches prepass");
    }
    // Execute an actual depth-dependent downstream compute consumer.
    const char* consume=R"HLSL(
        Texture2D<float4> Color:register(t0);Texture2D<float> Depth:register(t1);
        RWTexture2D<float4> Output:register(u0);
        [numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) {
            uint w,h;Color.GetDimensions(w,h);if(p.x>=w||p.y>=h)return;
            float z=Depth.Load(int3(p.xy,0));float4 c=Color.Load(int3(p.xy,0));
            Output[p.xy]=(z>0.3 && z<0.5 && c.a==1.0)?c:float4(1,0,1,0);
        })HLSL";
    auto consumeCode=Compile(consume,"main","cs_5_0");ComPtr<ID3D11ComputeShader> consumer;
    HR(dev->CreateComputeShader(consumeCode->GetBufferPointer(),consumeCode->GetBufferSize(),nullptr,&consumer));
    D3D11_TEXTURE2D_DESC outDesc{};color->GetDesc(&outDesc);outDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> out;ComPtr<ID3D11UnorderedAccessView> outUAV;
    HR(dev->CreateTexture2D(&outDesc,nullptr,&out));HR(dev->CreateUnorderedAccessView(out.Get(),nullptr,&outUAV));
    D3D11_SHADER_RESOURCE_VIEW_DESC depthRead{};depthRead.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;depthRead.Texture2D.MipLevels=1;
    depthRead.Format=d24?DXGI_FORMAT_R24_UNORM_X8_TYPELESS:DXGI_FORMAT_R32_FLOAT;
    ComPtr<ID3D11ShaderResourceView> cleanDepthSRV;HR(dev->CreateShaderResourceView(depth.Get(),&depthRead,&cleanDepthSRV));
    Prepare(true);Bind();ctx->Draw(3,0);ctx->OMSetRenderTargets(0,nullptr,nullptr);
    ID3D11ShaderResourceView* consumerInputs[]={colorSRV.Get(),cleanDepthSRV.Get()};
    ctx->CSSetShaderResources(0,2,consumerInputs);auto* destination=outUAV.Get();ctx->CSSetUnorderedAccessViews(0,1,&destination,nullptr);
    ctx->CSSetShader(consumer.Get(),nullptr,0);ctx->Dispatch((width+7)/8,(height+7)/8,1);
    Require(Read(dev.Get(),ctx.Get(),out.Get())==expectedColor,"downstream compute consumed sparse color or damaged depth");
    ID3D11UnorderedAccessView* noUav=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&noUav,nullptr);
    ID3D11ShaderResourceView* noInputs[2]{};ctx->CSSetShaderResources(0,2,noInputs);ctx->CSSetShader(nullptr,nullptr,0);
    const UINT indices[3]={0,1,2};D3D11_BUFFER_DESC indexDesc{};indexDesc.ByteWidth=sizeof(indices);indexDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{};indexData.pSysMem=indices;ComPtr<ID3D11Buffer> indexBuffer;
    HR(dev->CreateBuffer(&indexDesc,&indexData,&indexBuffer));
    const UINT indirectArgs[5]={3,1,0,0,0};indexDesc.ByteWidth=sizeof(indirectArgs);indexDesc.BindFlags=0;indexDesc.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    indexData.pSysMem=indirectArgs;ComPtr<ID3D11Buffer> indirect;HR(dev->CreateBuffer(&indexDesc,&indexData,&indirect));
    for(unsigned variant=0;variant<5;++variant) {
        Prepare(true);Bind();ctx->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R32_UINT,0);
        if(variant==0)ctx->DrawIndexed(3,0,0);
        if(variant==1)ctx->DrawIndexedInstanced(3,1,0,0,0);
        if(variant==2)ctx->DrawInstanced(3,1,0,0);
        if(variant==3)ctx->DrawIndexedInstancedIndirect(indirect.Get(),0);
        if(variant==4)ctx->DrawInstancedIndirect(indirect.Get(),0);
        Require(rdm.Stats().maskedDraws==1,"indexed/instanced/indirect draw hook missed scene");CheckColor();
        Require(Read(dev.Get(),ctx.Get(),depth.Get())==expectedDepth,"indexed draw altered original depth");
    }
    Prepare(true);Bind();ctx->Draw(3,0);
    ctx->PSSetShader(alpha.Get(),nullptr,0);ctx->Draw(3,0);
    auto before=rdm.Stats().maskedDraws;ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==before+1,"RDM failed to resume after protected alpha pass");CheckColor();
    // The second MRT must not be reconstructed when the shader writes only t0.
    ctx->PSSetShader(one.Get(),nullptr,0);before=rdm.Stats().maskedDraws;ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==before,"shader with incomplete MRT outputs was masked");
    // Depth-writing draws are always full rate and remain visible to consumers.
    Bind(false);ctx->Draw(3,0);Require(rdm.Stats().maskedDraws==before,"depth producer was masked");
    Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
    Bind();ds.DepthFunc=D3D11_COMPARISON_EQUAL;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    ComPtr<ID3D11DepthStencilState> equalWrite;HR(dev->CreateDepthStencilState(&ds,&equalWrite));
    ctx->OMSetDepthStencilState(equalWrite.Get(),0);before=rdm.Stats().maskedDraws;ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==before+1,"depth-equal color with writes enabled was rejected");
    Require(Read(dev.Get(),ctx.Get(),depth.Get())==expectedDepth,"equal color pass changed original depth");
    rdm.EndFrame();
    // CB subranges must survive both the private-mask pass and color resolve.
    ComPtr<ID3D11DeviceContext1> ctx1;HR(ctx.As(&ctx1));
    Prepare(true);Bind();
    std::array<float,128> cbData{};cbData[0]=0.9f;cbData[1]=1;
    cbData[64]=0.4f;cbData[65]=float(width);
    D3D11_BUFFER_DESC rangedDesc{};rangedDesc.ByteWidth=sizeof(cbData);rangedDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA rangedData{};rangedData.pSysMem=cbData.data();
    ComPtr<ID3D11Buffer> ranged;HR(dev->CreateBuffer(&rangedDesc,&rangedData,&ranged));
    ID3D11Buffer* rangedRaw=ranged.Get();UINT firstConstant=16,numConstants=16;
    ctx1->VSSetConstantBuffers1(0,1,&rangedRaw,&firstConstant,&numConstants);
    ctx1->PSSetConstantBuffers1(0,1,&rangedRaw,&firstConstant,&numConstants);
    ctx1->CSSetConstantBuffers1(0,1,&rangedRaw,&firstConstant,&numConstants);
    ctx->PSSetShader(strippedPS.Get(),nullptr,0);ctx->Draw(3,0);CheckColor();
    UINT firstOut=0,countOut=0;ComPtr<ID3D11Buffer> restored;
    ctx1->VSGetConstantBuffers1(0,1,&restored,&firstOut,&countOut);
    Require(restored.Get()==ranged.Get() && firstOut==16 && countOut==16,"VS constant-buffer range lost");
    restored.Reset();ctx1->PSGetConstantBuffers1(0,1,&restored,&firstOut,&countOut);
    Require(restored.Get()==ranged.Get() && firstOut==16 && countOut==16,"PS constant-buffer range lost");
    restored.Reset();ctx1->CSGetConstantBuffers1(0,1,&restored,&firstOut,&countOut);
    Require(restored.Get()==ranged.Get() && firstOut==16 && countOut==16,"CS constant-buffer range lost");
    rdm.EndFrame();
    // Recorded work resolves pending MRTs before execution and never inherits
    // the private mask, regardless of the command-list restore mode.
    ComPtr<ID3D11DeviceContext> deferred;HR(dev->CreateDeferredContext(0,&deferred));
    ComPtr<ID3D11CommandList> list;HR(deferred->FinishCommandList(FALSE,&list));
    for(BOOL restore : {FALSE,TRUE}) {
        Prepare(true);Bind();ctx->Draw(3,0);
        ctx->ExecuteCommandList(list.Get(),restore);
        Require(rdm.Stats().resolves==2,"command-list handoff missed MRTs");
        Bind();before=rdm.Stats().maskedDraws;ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==before,"command list retained stale ownership");CheckColor();
        Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==before+1,"RDM did not recover after a fresh depth prepass");CheckColor();
    }
    Prepare(true);Bind();ctx->Draw(3,0);ctx->ClearState();rdm.EndFrame();
    UINT vpCount=16;D3D11_VIEWPORT emptyVp[16]{};ctx->RSGetViewports(&vpCount,emptyVp);
    Require(vpCount==0,"resolve changed cleared viewport state");CheckColor();
    // A new surface cutting through a cluster keeps the whole cluster intact.
    Prepare(true);D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;raster.ScissorEnable=TRUE;
    ComPtr<ID3D11RasterizerState> scissor;HR(dev->CreateRasterizerState(&raster,&scissor));
    Bind(false);ctx->RSSetState(scissor.Get());D3D11_RECT rect{3,0,5,LONG(height)};ctx->RSSetScissorRects(1,&rect);
    params[0]=0.2f;ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);ctx->Draw(3,0);
    params[0]=0.4f;ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);Bind();
    auto edgeDepth=Read(dev.Get(),ctx.Get(),depth.Get());Bind();ctx->Draw(3,0);rdm.EndFrame();
    auto edgeColor=Read(dev.Get(),ctx.Get(),color.Get());
    for(UINT y=0;y<height;++y) for(UINT x=0;x<8;++x) Require(edgeColor[(y*width+x)*4+3]==255,"silhouette cluster was reconstructed from missing coverage");
    Require(Read(dev.Get(),ctx.Get(),depth.Get())==edgeDepth,"edge preservation altered depth");
    // Actual separate geometry draws at exactly the same depth. Thin modded
    // meshes cross reconstruction clusters; depth alone cannot identify them.
    auto meshVSCode=Compile(shader,"MeshVS","vs_5_0"),meshPSCode=Compile(shader,"MeshPS","ps_5_0");
    ComPtr<ID3D11VertexShader> meshVS;ComPtr<ID3D11PixelShader> meshPS;
    HR(dev->CreateVertexShader(meshVSCode->GetBufferPointer(),meshVSCode->GetBufferSize(),nullptr,&meshVS));
    HR(dev->CreatePixelShader(meshPSCode->GetBufferPointer(),meshPSCode->GetBufferSize(),nullptr,&meshPS));
    auto Meshes=[&](bool colors) {
        Bind(!colors?false:true);ctx->VSSetShader(meshVS.Get(),nullptr,0);
        ctx->PSSetShader(colors?meshPS.Get():nullptr,nullptr,0);
        const UINT cuts[]={0,3,5,width/2+3,width/2+5,width};
        Wrapped::expected=depthView.Get();Wrapped::checkDepthGuide=!colors;
        for(unsigned i=0;i+1<std::size(cuts);++i) {
            params[0]=.4f;params[2]=float(cuts[i]);params[3]=float(cuts[i+1]);
            ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);ctx->Draw(6,0);
        }
        Wrapped::checkDepthGuide=false;
    };
    Wrapped::acceptedDepth=0;Prepare();Meshes(false);Meshes(true);
    auto meshColor=Read(dev.Get(),ctx.Get(),color.Get()),meshMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    auto meshDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Prepare(true);Meshes(false);Meshes(true);rdm.EndFrame();
    Require(rdm.Stats().guideDraws==6 && rdm.Stats().maskedDraws==5,"mesh ownership fixture did not exercise native guide/masked draws");
    Require(Read(dev.Get(),ctx.Get(),color.Get())==meshColor,"coplanar mesh color bled across draw boundary");
    Require(Read(dev.Get(),ctx.Get(),motion.Get())==meshMotion,"coplanar mesh motion bled across draw boundary");
    Require(Read(dev.Get(),ctx.Get(),depth.Get())==meshDepth,"ownership capture changed original depth/stencil");
    Require(Wrapped::acceptedDepth==10,"depth-prepass observers missed original draws");
    // Alpha/discard depth producers must not leave a plausible stale owner at
    // the same z. Invalidation affects their geometry, and a fresh prepass recovers.
    Prepare(true);Bind(false);ctx->PSSetShader(alpha.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().guideInvalidationDraws==1,"alpha depth producer did not invalidate its geometry");
    Bind();Require(rdm.BeforeDraw(),"guide invalidation unexpectedly disabled the whole renderer");
    ctx->Draw(3,0);auto invalidated=Read(dev.Get(),ctx.Get(),color.Get());rdm.AfterDraw(true);rdm.EndFrame();
    for(size_t i=3;i<invalidated.size();i+=4)Require(invalidated[i]==255,"unsupported depth producer retained sparse coverage");
    for(unsigned mutation=0;mutation<(d24?3u:4u);++mutation) {
        Prepare(true);
        if(mutation==0)ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,.4f,0);
        if(mutation==1) {
            D3D11_TEXTURE2D_DESC dd{};depth->GetDesc(&dd);ComPtr<ID3D11Texture2D> copy;
            HR(dev->CreateTexture2D(&dd,nullptr,&copy));ctx->CopyResource(copy.Get(),depth.Get());ctx->CopyResource(depth.Get(),copy.Get());
        }
        if(mutation==2)ctx1->DiscardView(depthView.Get());
        if(mutation==3)ctx1->ClearView(depthView.Get(),params,nullptr,0);
        Bind();auto maskedBefore=rdm.Stats().maskedDraws;ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==maskedBefore,"depth mutation left stale ownership enabled");
        Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);Bind();ctx->Draw(3,0);
        Require(rdm.Stats().maskedDraws==maskedBefore+1,"fresh depth draw did not restore reconstruction");
    }
    params[2]=params[3]=0;ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);
    // Exercise CSX's mixed G-buffer format family together, using main depth
    // to scope intermediate targets that differ from submitted eye color.
    Prepare();
    const char* gbufferShader=R"HLSL(
        struct O {float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;
                  float4 d:SV_Target3;float4 e:SV_Target4;float4 f:SV_Target5;};
        O main() {O o;o.a=float4(.2,.4,.6,1);o.b=float4(.5,1,2,1);o.c=float4(.3,.6,.9,1);
                  o.d=float4(.25,.5,.75,1);o.e=float4(1,2,3,1);o.f=.6;return o;})HLSL";
    auto gcode=Compile(gbufferShader,"main","ps_5_0");ComPtr<ID3D11PixelShader> gps;
    HR(dev->CreatePixelShader(gcode->GetBufferPointer(),gcode->GetBufferSize(),nullptr,&gps));
    std::array<ComPtr<ID3D11Texture2D>,6> gbuffers;
    std::array<ComPtr<ID3D11RenderTargetView>,6> gviews;
    ID3D11RenderTargetView* graw[6]{};
    std::array<std::vector<unsigned char>,6> greference;
    const DXGI_FORMAT formats[6]={DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R11G11B10_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16_UNORM};
    D3D11_TEXTURE2D_DESC gdesc{};color->GetDesc(&gdesc);
    for(unsigned i=0;i<6;++i) {
        gdesc.Format=formats[i];HR(dev->CreateTexture2D(&gdesc,nullptr,&gbuffers[i]));
        HR(dev->CreateRenderTargetView(gbuffers[i].Get(),nullptr,&gviews[i]));graw[i]=gviews[i].Get();
    }
    ctx->OMSetRenderTargets(6,graw,depthView.Get());ctx->PSSetShader(gps.Get(),nullptr,0);ctx->Draw(3,0);
    for(unsigned i=0;i<6;++i) greference[i]=Read(dev.Get(),ctx.Get(),gbuffers[i].Get());
    const float clear[4]{};for(auto* view:graw)ctx->ClearRenderTargetView(view,clear);
    Prepare(true);ctx->OMSetRenderTargets(6,graw,depthView.Get());ctx->PSSetShader(gps.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==1,"CSX-format G-buffer draw was not masked");
    for(unsigned i=0;i<6;++i) Require(Read(dev.Get(),ctx.Get(),gbuffers[i].Get())==greference[i],"CSX G-buffer output changed after handoff");
    Require(rdm.Stats().resolves==6,"not all six G-buffers were resolved before consumption");
    Require(Read(dev.Get(),ctx.Get(),depth.Get())==expectedDepth,"CSX G-buffer handoff altered depth");
    rdm.EndFrame();
    Require(Wrapped::accepted>=8,"post-draw callback fixture did not run");
    if(info) {
        unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> data(size);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());HR(info->GetMessage(i,m,&size));
            if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::printf("D3D11 WARNING/ERROR: %s\n",m->pDescription);++errors; }
        }
        Require(errors==0,"D3D11 debug layer reported errors");
    }
    std::printf("RDM HANDOFF PASS %ux%u %s auxiliaryFormat=%u debug=%u submitted=%s\n",width,height,d24?"D24S8":"D32",unsigned(auxiliaryFormat),flags!=0,
        separateSubmittedEyes?"separate eyes / depth ownership":"shared color");
}
int main(int argc,char** argv) {
    try {
        if(argc>1 && std::strcmp(argv[1],"--large")==0) {
            ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
            for(UINT i=0;;++i) {
                ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));
                if(desc.VendorId==0x10DE) {
                    std::printf("Large target adapter %ls\n",desc.Description);
                    Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,6800,3468,true);
                    std::puts("LARGE RDM HANDOFF TEST PASS");return 0;
                }
            }
            throw std::runtime_error("large-target test requires a hardware adapter");
        }
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,false);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,134,70,true);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,false,DXGI_FORMAT_R8G8B8A8_UNORM,true);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,134,70,true,DXGI_FORMAT_R8G8B8A8_UNORM,true);
        for(auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT})
            Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,format);
        ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            std::printf("Adapter %ls vendor=%04X\n",desc.Description,desc.VendorId);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true);
        }
        std::puts("ALL RDM HANDOFF TESTS PASS");return 0;
    }catch(const std::exception& e) {std::printf("FAIL: %s\n",e.what());return 1;}
}
