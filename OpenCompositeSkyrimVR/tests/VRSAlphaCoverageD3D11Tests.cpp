// Actual production cutout-material policy and NVIDIA GPU regression tests.
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/VRSManager.h"
#include "OpenOVR/Compositor/VRSAlphaCoverageScope.h"
#include "OpenOVR/Compositor/RDMRenderScope.h"
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
static VRSAlphaCoverageScope* coverage=nullptr;
static unsigned toggles=0;
static bool applied=false;
void Changed(ID3D11DeviceContext* context,bool targetsChanged) {
    if(coverage) coverage->ShaderStateChanged(context,targetsChanged);
    ID3D11RenderTargetView* views[8]{}; ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(8,views,&depth);
    ComPtr<ID3D11PixelShader> shader; context->PSGetShader(&shader,nullptr,nullptr);
    const auto* viewports=ocu_vrs_guard::CurrentViewports(context);
    const bool scene=sceneScope->Matches(context,8,views,depth.Get());
    const bool eligible=scene && viewports && manager->UpdateActiveViewports(viewports->count,viewports->values)
        && ocu_vrs_guard::CurrentReasons(context)==ocu_vrs_guard::Compatible
        && ocu_vrs_guard::CurrentCoarseHazards(context)==ocu_vrs_guard::CoarseCompatible
        && (!coverage || !coverage->ProtectsCurrentDraw(context));
    for(auto* view:views) if(view) view->Release();
    if(targetsChanged && applied) { manager->Disable(); applied=false; }
    if(eligible && !applied) { applied=manager->ApplyStereo(); ++toggles; }
    if(!eligible && applied) { manager->Disable(); applied=false; ++toggles; }
}

void CoverageChanged(ID3D11DeviceContext* context) { Changed(context,false); }

// Independent post-draw observer, outside the production native hook broker.
// It models a renderer consuming the game bindings immediately after Draw.
// No RDM instance is armed in this fixture mode; VRS may still use the broker.
struct UnarmedDrawObserver {
    using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT);
    static inline DrawFn draw=nullptr;
    static inline ID3D11Multithread* multithread=nullptr;
    static inline BOOL expectedProtection=FALSE;
    static inline UINT64 callbacks=0;
    ID3D11DeviceContext* context=nullptr;
    void** original=nullptr;
    std::array<void*,149> table{};
    template<class Interface> static bool AliasesContext(ID3D11DeviceContext* c) {
        ComPtr<Interface> queried;
        return SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&queried)))&&
            static_cast<ID3D11DeviceContext*>(queried.Get())==c;
    }
    static void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* c,UINT count,UINT first) {
        Check(!RDMRenderScope::Active(c),"RDM unexpectedly active in VRS/disabled fixture");
        ComPtr<ID3D11RenderTargetView> beforeColor,afterColor;
        ComPtr<ID3D11DepthStencilView> beforeDepth,afterDepth;
        ComPtr<ID3D11PixelShader> beforePS,afterPS;
        c->OMGetRenderTargets(1,&beforeColor,&beforeDepth);c->PSGetShader(&beforePS,nullptr,nullptr);
        draw(c,count,first);
        c->OMGetRenderTargets(1,&afterColor,&afterDepth);c->PSGetShader(&afterPS,nullptr,nullptr);
        Check(beforeColor.Get()==afterColor.Get()&&beforeDepth.Get()==afterDepth.Get()&&beforePS.Get()==afterPS.Get(),
            "inactive RDM bootstrap altered bindings seen by post-draw observer");
        Check(!RDMRenderScope::Active(c)&&multithread->GetMultithreadProtected()==expectedProtection,
            "inactive RDM bootstrap armed a scope or changed game protection");
        ++callbacks;
    }
    UnarmedDrawObserver(ID3D11DeviceContext* c,ID3D11Multithread* mt,bool enabled) {
        if(!enabled)return;
        context=c;multithread=mt;expectedProtection=mt->GetMultithreadProtected();callbacks=0;
        original=*reinterpret_cast<void***>(context);
        // Counts verified against SDK10.0.26100.0 C-interface declarations.
        // Preserve extension methods only when QueryInterface aliases this
        // exact pointer; non-aliasing COM interfaces keep their own tables.
        size_t slots=115;
        if(AliasesContext<ID3D11DeviceContext1>(context))slots=134;
        if(AliasesContext<ID3D11DeviceContext2>(context))slots=144;
        if(AliasesContext<ID3D11DeviceContext3>(context))slots=147;
        if(AliasesContext<ID3D11DeviceContext4>(context))slots=149;
        std::copy_n(original,slots,table.begin());draw=reinterpret_cast<DrawFn>(table[13]);
        table[13]=reinterpret_cast<void*>(&Draw);*reinterpret_cast<void***>(context)=table.data();
    }
    ~UnarmedDrawObserver(){if(context)*reinterpret_cast<void***>(context)=original;}
};

int main(int argc,char** argv) try {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool unarmedBootstrap=false,protectedContext=false;
    for(int i=1;i<argc;++i){
        if(std::strcmp(argv[i],"--unarmed-bootstrap")==0)unarmedBootstrap=true;
        else if(std::strcmp(argv[i],"--protected")==0)protectedContext=true;
        else Check(false,"unknown fixture argument");
    }
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
    ComPtr<ID3D11Multithread> multithread;HR(context.As(&multithread));
    multithread->SetMultithreadProtected(protectedContext);
    Check(multithread->GetMultithreadProtected()==BOOL(protectedContext),"fixture protection mode applied");
    if(unarmedBootstrap){
        Check(RDMRenderScope::PrepareDrawHooks(device.Get()),"early RDM draw bootstrap installed for VRS/disabled test");
        Check(!RDMRenderScope::Active(context.Get()),"early draw bootstrap armed RDM");
        Check(multithread->GetMultithreadProtected()==BOOL(protectedContext),"bootstrap changed game protection");
    }
    UnarmedDrawObserver observer(context.Get(),multithread.Get(),unarmedBootstrap);
    Check(ocu_vrs_guard::InstallShaderCapture(device.Get()),"production shader capture installed");
    constexpr UINT width=256,height=128,texWidth=128;
    auto diffuse=Color(device.Get(),width,height,DXGI_FORMAT_R32G32B32A32_FLOAT);
    D3D11_TEXTURE2D_DESC dd{};dd.Width=width;dd.Height=height;dd.MipLevels=2;dd.ArraySize=1;
    dd.Format=DXGI_FORMAT_D32_FLOAT;dd.SampleDesc.Count=1;dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    HR(device->CreateTexture2D(&dd,nullptr,&depth));HR(device->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
    D3D11_TEXTURE2D_DESC td{};td.Width=texWidth;td.Height=height;td.MipLevels=1;td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> foliage;ComPtr<ID3D11ShaderResourceView> foliageSRV;
    HR(device->CreateTexture2D(&td,nullptr,&foliage));HR(device->CreateShaderResourceView(foliage.Get(),nullptr,&foliageSRV));
    const char* vsCode=R"(
struct Out{float4 p:SV_Position;float2 uv:TEXCOORD0;};
Out main(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);Out o;o.p=float4(uv*float2(2,-2)+float2(-1,1),0.5,1);o.uv=uv;return o;})";
    const char* depthCode=R"(
Texture2D<float4> baseTexture:register(t0);SamplerState baseSampler:register(s0);
void main(float4 p:SV_Position,float2 uv:TEXCOORD0){float alpha=baseTexture.SampleBias(baseSampler,uv,0).a;if(alpha<0.5)discard;})";
    // RunGrass color pass uses RGB while alpha rejection belongs to depth prepass.
    const char* colorCode=R"(
Texture2D<float4> baseTexture:register(t0);SamplerState baseSampler:register(s0);
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(baseTexture.SampleBias(baseSampler,uv,0).rgb,1);})";
    const char* opaqueCode="float4 main(float4 p:SV_Position):SV_Target{return float4(p.xy,0.25,1);}";
    auto vsBytes=Compile(vsCode,"vs_5_0"),depthBytes=Compile(depthCode,"ps_5_0"),colorBytes=Compile(colorCode,"ps_5_0"),opaqueBytes=Compile(opaqueCode,"ps_5_0");
    ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> depthPS,colorPS,opaquePS;
    HR(device->CreateVertexShader(vsBytes->GetBufferPointer(),vsBytes->GetBufferSize(),nullptr,&vs));
    HR(device->CreatePixelShader(depthBytes->GetBufferPointer(),depthBytes->GetBufferSize(),nullptr,&depthPS));
    HR(device->CreatePixelShader(colorBytes->GetBufferPointer(),colorBytes->GetBufferSize(),nullptr,&colorPS));
    HR(device->CreatePixelShader(opaqueBytes->GetBufferPointer(),opaqueBytes->GetBufferSize(),nullptr,&opaquePS));
    std::printf("PRODUCTION_GUARD depth=0x%X color=0x%X opaque=0x%X\n",ocu_vrs_guard::ShaderReasons(depthPS.Get()),ocu_vrs_guard::ShaderReasons(colorPS.Get()),ocu_vrs_guard::ShaderReasons(opaquePS.Get()));
    Check(ocu_vrs_guard::ShaderReasons(depthPS.Get())&ocu_vrs_guard::Discard,"grass depth cutoff is recognized");
    Check(ocu_vrs_guard::ShaderReasons(colorPS.Get())==ocu_vrs_guard::Compatible,"RGB-only grass color has no protected shader instruction");
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> point;HR(device->CreateSamplerState(&sd,&point));auto* sampler=point.Get();context->PSSetSamplers(0,1,&sampler);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rd,&raster));context->RSSetState(raster.Get());
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> writeDepth,equalDepth;
    HR(device->CreateDepthStencilState(&ds,&writeDepth));ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;ds.DepthFunc=D3D11_COMPARISON_EQUAL;
    HR(device->CreateDepthStencilState(&ds,&equalDepth));
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);context->OMSetBlendState(nullptr,nullptr,~0u);
    auto draw=[&]() {
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;HR(device->CreateQuery(&qd,&query));context->Begin(query.Get());
        const auto callbacksBefore=UnarmedDrawObserver::callbacks;
        for(int eye=0;eye<2;++eye){D3D11_VIEWPORT vp{float(eye*width/2),0,float(width/2),float(height),0,1};context->RSSetViewports(1,&vp);context->Draw(3,0);}
        if(unarmedBootstrap)Check(UnarmedDrawObserver::callbacks==callbacksBefore+2,
            "early RDM hooks lost or duplicated stereo post-draw callbacks");
        context->End(query.Get());D3D11_QUERY_DATA_PIPELINE_STATISTICS s{};HRESULT hr=S_FALSE;
        for(int i=0;i<1000&&hr==S_FALSE;++i){hr=context->GetData(query.Get(),&s,sizeof(s),0);if(hr==S_FALSE)Sleep(1);}Check(hr==S_OK,"GPU statistics completed");return s.PSInvocations;
    };
    auto read=[&]() {
        D3D11_TEXTURE2D_DESC d{};diffuse.texture->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),diffuse.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<std::array<float,4>> result(width*height);
        for(UINT y=0;y<height;++y)std::memcpy(result.data()+y*width,static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch,width*16);
        context->Unmap(staging.Get(),0);return result;
    };
    if(unarmedBootstrap){
        // Foveation completely disabled: pixel coordinates expose any accidental
        // coarse shading/reconstruction, and the PS count catches lost/replayed
        // work. This runs before VRS is initialized or its draw observer armed.
        auto* target=diffuse.rtv.Get();context->OMSetRenderTargets(1,&target,dsv.Get());
        context->OMSetDepthStencilState(writeDepth.Get(),0);context->PSSetShader(opaquePS.Get(),nullptr,0);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        const auto fullInvocations=draw();const auto pixels=read();
        Check(fullInvocations==UINT64(width)*height,"inactive RDM changed full-rate pixel invocation count");
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const auto& p=pixels[y*width+x];
            Check(p[0]==float(x)+0.5f&&p[1]==float(y)+0.5f&&p[2]==0.25f&&p[3]==1,
                "inactive RDM changed exact full-rate stereo pixels");}
        D3D11_TEXTURE2D_DESC stagedDesc=dd;stagedDesc.BindFlags=0;stagedDesc.Usage=D3D11_USAGE_STAGING;
        stagedDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> depthRead;
        HR(device->CreateTexture2D(&stagedDesc,nullptr,&depthRead));context->CopyResource(depthRead.Get(),depth.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(depthRead.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(UINT y=0;y<height;++y){const auto* row=reinterpret_cast<const float*>(
            static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch);
            for(UINT x=0;x<width;++x)Check(row[x]==0.5f,"inactive RDM changed original game depth");}
        context->Unmap(depthRead.Get(),0);
        std::printf("UNARMED DISABLED PASS multithread=%d PS=%llu callbacks=%llu exactStereoPixels=yes originalDepth=yes\n",
            int(protectedContext),fullInvocations,UnarmedDrawObserver::callbacks);
    }
    VRSAlphaCoverageScope alphaCoverage;
    VRSManager vrs;Check(vrs.Initialize(device.Get()),"production NVAPI backend initialized");manager=&vrs;
    ocu_vrs_scope::SceneScope scope;scope.Arm(context.Get(),depth.Get(),diffuse.texture.Get(),width,height);sceneScope=&scope;
    struct Result{UINT surviving=0,wrong=0,backgroundChanged=0;UINT wrongLeft=0,wrongRight=0;UINT64 ps=0;};
    auto run=[&](int axis,int phase,ocu_foveation::Rate rate,bool protect) {
        alphaCoverage.EndFrame();coverage=nullptr;ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;
        if(protect){coverage=&alphaCoverage;Check(alphaCoverage.Arm(context.Get(),depth.Get(),&CoverageChanged),"production alpha material scope armed");}
        std::vector<unsigned char> texels(texWidth*height*4);
        auto inside=[&](UINT x,UINT y){return ((axis==0?x:axis==1?y:x+y)+phase)%4==0;};
        for(UINT y=0;y<height;++y)for(UINT x=0;x<texWidth;++x){bool leaf=inside(x,y);auto* t=texels.data()+(y*texWidth+x)*4;t[0]=t[2]=leaf?0:255;t[1]=255;t[3]=leaf?255:0;}
        context->UpdateSubresource(foliage.Get(),0,nullptr,texels.data(),texWidth*4,0);auto* srv=foliageSRV.Get();context->PSSetShaderResources(0,1,&srv);
        Check(vrs.UpdateStereoPattern(width,height,{0,0,width/2,height},{width/2,0,width/2,height},0.3f,0.6f,{ocu_foveation::Rate::X1x1,rate,rate}),"pattern ready");
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->OMSetRenderTargets(0,nullptr,dsv.Get());context->OMSetDepthStencilState(writeDepth.Get(),0);context->PSSetShader(depthPS.Get(),nullptr,0);
        Check(ocu_vrs_guard::WatchContext(context.Get(),&Changed),"production state hooks active");Changed(context.Get(),true);Check(!applied,"depth prepass remains full rate");draw();
        const float blue[4]={0.2f,0.35f,0.8f,1};context->ClearRenderTargetView(diffuse.rtv.Get(),blue);auto* rtv=diffuse.rtv.Get();context->OMSetRenderTargets(1,&rtv,dsv.Get());context->OMSetDepthStencilState(equalDepth.Get(),0);context->PSSetShader(colorPS.Get(),nullptr,0);Changed(context.Get(),true);
        Result r;r.ps=draw();auto pixels=read();
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){auto p=pixels[y*width+x];if(inside(x%texWidth,y)){++r.surviving;if(p[0]>0.01f||p[1]<0.99f||p[2]>0.01f){++r.wrong;if(x<width/2)++r.wrongLeft;else++r.wrongRight;}}else if(std::fabs(p[0]-blue[0])>0.001f||std::fabs(p[1]-blue[1])>0.001f||std::fabs(p[2]-blue[2])>0.001f)++r.backgroundChanged;}
        std::printf("GRASS axis=%d phase=%d rate=%s productionPolicy=%d PS=%llu surviving=%u wrong=%u wrongL=%u wrongR=%u backgroundChanged=%u\n",axis,phase,ocu_foveation::RateName(rate),int(protect),r.ps,r.surviving,r.wrong,r.wrongLeft,r.wrongRight,r.backgroundChanged);
        if(protect){const auto st=alphaCoverage.Stats();Check(st.depthDraws==2&&st.materials==1&&st.protectedDraws==2,"actual policy learned and protected both eyes");}
        alphaCoverage.EndFrame();coverage=nullptr;ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;return r;
    };
    UINT horizontalFailure=0,verticalFailure=0;
    for(int axis=0;axis<3;++axis)for(int phase=0;phase<2;++phase){
        auto full=run(axis,phase,ocu_foveation::Rate::X1x1,false);Check(full.wrong==0&&full.backgroundChanged==0,"full rate matches texture cutout coverage");
        for(auto rate:{ocu_foveation::Rate::X2x1,ocu_foveation::Rate::X1x2,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X4x4}){
            auto coarse=run(axis,phase,rate,false);auto fixed=run(axis,phase,rate,true);
            Check(coarse.backgroundChanged==0,"coarse color retains correct raster/depth coverage");Check(fixed.wrong==0&&fixed.backgroundChanged==0,"production material scope fixes surviving leaf material");
            if(rate==ocu_foveation::Rate::X2x1)horizontalFailure+=coarse.wrong;if(rate==ocu_foveation::Rate::X1x2)verticalFailure+=coarse.wrong;
        }
    }
    Check(horizontalFailure>0&&verticalFailure>0,"grass color corruption reproduced in both VRS axes");
    context->OMSetDepthStencilState(writeDepth.Get(),0);auto* rtv=diffuse.rtv.Get();context->OMSetRenderTargets(1,&rtv,dsv.Get());context->PSSetShader(opaquePS.Get(),nullptr,0);
    const auto fullOpaque=draw();coverage=&alphaCoverage;Check(alphaCoverage.Arm(context.Get(),depth.Get(),&CoverageChanged),"opaque scope armed");Check(ocu_vrs_guard::WatchContext(context.Get(),&Changed),"opaque control hook");Changed(context.Get(),true);const auto coarseOpaque=draw();
    std::printf("OPAQUE fullPS=%llu coarsePS=%llu saved=%.2f%%\n",fullOpaque,coarseOpaque,100.*(1.-double(coarseOpaque)/fullOpaque));Check(coarseOpaque<fullOpaque*.8,"opaque retains VRS shading reduction");

    // State/lifecycle regressions use the real D3D Draw detours, not direct
    // BeforeDraw calls or a fixture-only shader pointer exemption.
    ComPtr<ID3D11ShaderResourceView> aliasSRV;HR(device->CreateShaderResourceView(foliage.Get(),nullptr,&aliasSRV));
    ComPtr<ID3D11Texture2D> otherMaterial,otherDepth;ComPtr<ID3D11ShaderResourceView> otherSRV;
    ComPtr<ID3D11DepthStencilView> otherDSV,mip1DSV,readOnlyDSV;
    HR(device->CreateTexture2D(&td,nullptr,&otherMaterial));HR(device->CreateShaderResourceView(otherMaterial.Get(),nullptr,&otherSRV));
    HR(device->CreateTexture2D(&dd,nullptr,&otherDepth));HR(device->CreateDepthStencilView(otherDepth.Get(),nullptr,&otherDSV));
    D3D11_DEPTH_STENCIL_VIEW_DESC mipDesc{};mipDesc.Format=dd.Format;mipDesc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;mipDesc.Texture2D.MipSlice=1;
    HR(device->CreateDepthStencilView(depth.Get(),&mipDesc,&mip1DSV));mipDesc.Texture2D.MipSlice=0;mipDesc.Flags=D3D11_DSV_READ_ONLY_DEPTH;
    HR(device->CreateDepthStencilView(depth.Get(),&mipDesc,&readOnlyDSV));
    auto pixelShader=[&](const char* source){auto b=Compile(source,"ps_5_0");ComPtr<ID3D11PixelShader> p;HR(device->CreatePixelShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&p));return p;};
    auto pbrPS=pixelShader(R"(Texture2D<float4> base:register(t0);Texture2D<float4> normal:register(t1);SamplerState s:register(s0);
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(base.Sample(s,uv).rgb*0.9+normal.Sample(s,uv).rgb*0.1,1);})");
    auto shiftedPS=pixelShader(R"(Texture2D<float4> base:register(t1);Texture2D<float4> normal:register(t2);SamplerState s:register(s0);
float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(base.Sample(s,uv).rgb*0.9+normal.Sample(s,uv).rgb*0.1,1);})");
    auto multiDepthPS=pixelShader(R"(Texture2D<float4> base:register(t0);Texture2D<float4> normal:register(t1);SamplerState s:register(s0);
void main(float4 p:SV_Position,float2 uv:TEXCOORD0){if(base.Sample(s,uv).a*normal.Sample(s,uv).a<0.5)discard;})");
    auto common=[&](ID3D11DeviceContext* c){
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);c->VSSetShader(vs.Get(),nullptr,0);c->RSSetState(raster.Get());
        c->PSSetSamplers(0,1,&sampler);c->OMSetBlendState(nullptr,nullptr,~0u);
        D3D11_VIEWPORT vp{0,0,float(width/2),float(height),0,1};c->RSSetViewports(1,&vp);
    };
    auto beginFrame=[&](){alphaCoverage.EndFrame();coverage=nullptr;ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;
        common(context.Get());coverage=&alphaCoverage;Check(alphaCoverage.Arm(context.Get(),depth.Get(),&CoverageChanged),"lifecycle scope arm");
        Check(ocu_vrs_guard::WatchContext(context.Get(),&Changed),"lifecycle watch");
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
    };
    auto depthDraw=[&](ID3D11DepthStencilView* d,UINT count=3){
        context->OMSetRenderTargets(0,nullptr,d);context->OMSetDepthStencilState(writeDepth.Get(),0);context->PSSetShader(depthPS.Get(),nullptr,0);
        auto* srv=foliageSRV.Get();context->PSSetShaderResources(0,1,&srv);context->Draw(count,0);
    };
    auto colorDraw=[&](ID3D11ShaderResourceView* material,ID3D11PixelShader* ps){
        auto* rt=diffuse.rtv.Get();context->OMSetRenderTargets(1,&rt,dsv.Get());context->OMSetDepthStencilState(equalDepth.Get(),0);
        ID3D11ShaderResourceView* resources[]={material,otherSRV.Get(),otherSRV.Get()};context->PSSetShaderResources(0,3,resources);
        context->PSSetShader(ps,nullptr,0);context->Draw(3,0);
    };
    beginFrame();depthDraw(dsv.Get());colorDraw(aliasSRV.Get(),colorPS.Get());
    Check(alphaCoverage.ProtectsCurrentDraw(context.Get())&&!applied,"distinct SRV alias of learned texture protected");
    auto before=alphaCoverage.Stats();auto beforeToggles=toggles;
    for(int i=0;i<1000;++i)context->Draw(3,0);
    Check(alphaCoverage.Stats().stateQueries==before.stateQueries&&toggles==beforeToggles,"1000 same-state protected draws add no queries or NVAPI toggles");
    Check(alphaCoverage.Stats().protectedDraws==before.protectedDraws+1000,"every protected draw counted");
    std::printf("CACHE protectedDraws=1000 extraQueries=0 extraToggles=0\n");
    colorDraw(aliasSRV.Get(),pbrPS.Get());Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"PBR base+normal color protected");
    context->PSSetShader(shiftedPS.Get(),nullptr,0);context->Draw(3,0);
    Check(!alphaCoverage.ProtectsCurrentDraw(context.Get())&&applied,"two-slot shader change invalidates sampled mask without resource setter");
    colorDraw(aliasSRV.Get(),colorPS.Get());auto* replacement=otherSRV.Get();context->PSSetShaderResources(0,1,&replacement);context->Draw(3,0);
    Check(!alphaCoverage.ProtectsCurrentDraw(context.Get())&&applied,"SRV-only switch restores opaque VRS before draw");
    colorDraw(aliasSRV.Get(),colorPS.Get());ComPtr<ID3D11RenderTargetView> materialRT;HR(device->CreateRenderTargetView(foliage.Get(),nullptr,&materialRT));
    auto* aliasTarget=materialRT.Get();context->OMSetRenderTargets(1,&aliasTarget,nullptr);
    ComPtr<ID3D11ShaderResourceView> actuallyBound;context->PSGetShaderResources(0,1,&actuallyBound);Check(!actuallyBound,"D3D implicitly unbound output-aliased material SRV");
    auto* sceneTarget=diffuse.rtv.Get();context->OMSetRenderTargets(1,&sceneTarget,dsv.Get());context->Draw(3,0);
    Check(!alphaCoverage.ProtectsCurrentDraw(context.Get())&&applied,"policy rereads SRV after implicit OM unbind instead of using stale identity");
    before=alphaCoverage.Stats();beforeToggles=toggles;for(int i=0;i<1000;++i)context->Draw(3,0);
    Check(alphaCoverage.Stats().stateQueries==before.stateQueries&&toggles==beforeToggles,"1000 same-state opaque draws add no queries or NVAPI toggles");
    std::printf("CACHE opaqueDraws=1000 extraQueries=0 extraToggles=0\n");
    colorDraw(aliasSRV.Get(),colorPS.Get());context->ClearDepthStencilView(mip1DSV.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);
    Check(alphaCoverage.ProtectsCurrentDraw(context.Get())&&alphaCoverage.Stats().materials==1,"same-resource mip1 clear preserves canonical mip0 tags");
    context->ClearDepthStencilView(readOnlyDSV.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);
    Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"invalid read-only depth clear preserves tags");
    context->ClearDepthStencilView(otherDSV.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);
    Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"foreign depth clear preserves tags");
    context->ClearState();common(context.Get());colorDraw(aliasSRV.Get(),colorPS.Get());
    Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"ClearState invalidates bindings but does not erase extant GPU depth coverage");
    context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);
    Check(!alphaCoverage.ProtectsCurrentDraw(context.Get())&&alphaCoverage.Stats().materials==0,"canonical depth clear removes old material tags");
    beginFrame();depthDraw(dsv.Get(),0);colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.Stats().depthDraws==0&&!alphaCoverage.ProtectsCurrentDraw(context.Get()),"zero count draw is not learned");
    depthDraw(otherDSV.Get());colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.Stats().materials==0&&!alphaCoverage.ProtectsCurrentDraw(context.Get()),"shadow depth does not tag scene material");
    depthDraw(readOnlyDSV.Get());colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.Stats().materials==0,"read-only depth producer is not learned");
    depthDraw(dsv.Get());colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.Stats().materials==1,"valid producer learned after excluded draws");
    beginFrame();colorDraw(aliasSRV.Get(),colorPS.Get());Check(!alphaCoverage.ProtectsCurrentDraw(context.Get())&&alphaCoverage.Stats().materials==0,"new frame releases old learned identities");
    // Predicate outcome can be unknown to the CPU. Both outcomes must eagerly
    // mark potential alpha depth, rather than skipping a predicate that passes.
    D3D11_QUERY_DESC predDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};ComPtr<ID3D11Predicate> predicate;HR(device->CreatePredicate(&predDesc,&predicate));
    context->Begin(predicate.Get());context->End(predicate.Get());
    for(BOOL condition:{FALSE,TRUE}){beginFrame();context->SetPredication(predicate.Get(),condition);depthDraw(dsv.Get());context->SetPredication(nullptr,FALSE);
        colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"predicated cutout depth conservatively learned for both outcomes");}
    beginFrame();context->OMSetRenderTargets(0,nullptr,dsv.Get());context->OMSetDepthStencilState(writeDepth.Get(),0);
    context->PSSetShader(multiDepthPS.Get(),nullptr,0);ID3D11ShaderResourceView* multiResources[]={foliageSRV.Get(),otherSRV.Get()};context->PSSetShaderResources(0,2,multiResources);context->Draw(3,0);
    Check(alphaCoverage.Stats().untrackedDepthDraws==1&&alphaCoverage.Stats().materials==0,"known multi-texture producer is explicitly counted as untracked");
    // Deferred recording is isolated; playback is intentionally conservative
    // because driver command lists need not replay CPU-side draw hooks.
    ComPtr<ID3D11DeviceContext> deferred;HR(device->CreateDeferredContext(0,&deferred));
    Check(!alphaCoverage.Arm(deferred.Get(),depth.Get(),&CoverageChanged),"deferred context cannot arm immediate-frame policy");
    beginFrame();common(deferred.Get());deferred->OMSetRenderTargets(0,nullptr,dsv.Get());deferred->OMSetDepthStencilState(writeDepth.Get(),0);
    deferred->PSSetShader(depthPS.Get(),nullptr,0);auto* base=foliageSRV.Get();deferred->PSSetShaderResources(0,1,&base);deferred->Draw(3,0);
    Check(alphaCoverage.Stats().depthDraws==0,"deferred recording never learns into immediate frame");
    ComPtr<ID3D11CommandList> list;HR(deferred->FinishCommandList(FALSE,&list));
    for(BOOL restore:{FALSE,TRUE}){beginFrame();context->ExecuteCommandList(list.Get(),restore);common(context.Get());colorDraw(aliasSRV.Get(),colorPS.Get());
        Check(alphaCoverage.ProtectsCurrentDraw(context.Get())&&alphaCoverage.Stats().unknownCommandLists==1&&alphaCoverage.Stats().ambiguousDraws==1,"command-list alpha coverage protects later equal color with either restore mode");
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);context->Draw(3,0);Check(!alphaCoverage.ProtectsCurrentDraw(context.Get()),"canonical clear ends command-list ambiguity");}
    // Alternating owners uses the same native hook broker; no duplicate-hook
    // failures or stale VRS observer calls may appear after switching back.
    if(!unarmedBootstrap){
    auto rdmColor=Color(device.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto rdmDepthDesc=dd;rdmDepthDesc.Format=DXGI_FORMAT_R32_TYPELESS;rdmDepthDesc.MipLevels=1;rdmDepthDesc.BindFlags|=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> rdmDepth;HR(device->CreateTexture2D(&rdmDepthDesc,nullptr,&rdmDepth));
    for(int i=0;i<3;++i){alphaCoverage.EndFrame();coverage=nullptr;ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();applied=false;
        RDMRenderScope rdm;float centers[4]={.5f,.5f,.5f,.5f};
        Check(rdm.Arm(context.Get(),rdmDepth.Get(),rdmColor.texture.Get(),width,height,{0,0,int(width/2),int(height)},{int(width/2),0,int(width/2),int(height)},{.3f,.6f,true,false,{}},centers),"VRS to RDM uses shared hooks");
        rdm.EndFrame();beginFrame();depthDraw(dsv.Get());colorDraw(aliasSRV.Get(),colorPS.Get());Check(alphaCoverage.ProtectsCurrentDraw(context.Get()),"RDM to VRS retains draw observation");}
    }
    std::printf("LIFECYCLE PASS: aliases, PBR, shader masks, cache, clear views, zero draws, shadow/read-only depth, predicates, deferred execution, %s\n",
        unarmedBootstrap?"RDM never armed":"backend switching");
    alphaCoverage.EndFrame();coverage=nullptr;
    ocu_vrs_guard::UnwatchContext(context.Get());vrs.Disable();
    Check(multithread->GetMultithreadProtected()==BOOL(protectedContext),"VRS lifecycle changed game protection");
    if(unarmedBootstrap){Check(!RDMRenderScope::Active(context.Get()),"RDM became active during VRS-only run");
        std::printf("UNARMED VRS PASS multithread=%d callbacks=%llu RDM-never-armed=yes opaqueVRS-reduction=yes cutoutPolicy=yes\n",
            int(protectedContext),UnarmedDrawObserver::callbacks);}
    std::puts("PASS: actual production cutout material policy fixes two-pass foliage while retaining opaque VRS");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
