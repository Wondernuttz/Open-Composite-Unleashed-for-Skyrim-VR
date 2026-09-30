#include <array>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cstring>
#include <string>
#include "OpenOVR/Compositor/VRSSceneScope.h"
#include "OpenOVR/Compositor/VRSManager.h"

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* msg) { std::puts(msg); }
void oovr_log_raw_format(const char*, long, const char*, const char* fmt, ...)
{
    va_list args; va_start(args, fmt); std::vprintf(fmt, args); va_end(args); std::puts("");
}
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void HR(HRESULT hr) { Require(SUCCEEDED(hr), "D3D11 call failed"); }

struct Target {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
};
static Target Color(ID3D11Device* device, UINT w, UINT h)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1;
    d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count=1;
    d.BindFlags=D3D11_BIND_RENDER_TARGET;
    Target t; HR(device->CreateTexture2D(&d,nullptr,&t.texture));
    HR(device->CreateRenderTargetView(t.texture.Get(),nullptr,&t.view));
    return t;
}

static VRSManager* watchedManager = nullptr;
static ocu_vrs_scope::SceneScope* watchedScope = nullptr;
static bool watchedApplied = false;
static unsigned protectedCommands = 0;
static void Changed(ID3D11DeviceContext* context, bool targetsChanged)
{
    if (!watchedManager || !watchedScope) return;
    ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &depth);
    const auto reasons = ocu_vrs_guard::CurrentReasons(context);
    if (reasons & ocu_vrs_guard::CommandList) ++protectedCommands;
    const bool scene = watchedScope->Matches(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, depth.Get());
    const auto* viewports = ocu_vrs_guard::CurrentViewports(context);
    const bool viewportReady = scene && viewports &&
        watchedManager->UpdateActiveViewports(viewports->count, viewports->values);
    const bool apply = !reasons && scene && viewportReady;
    for(auto* view: views) if(view) view->Release();
    if (targetsChanged && watchedApplied) { watchedManager->Disable(); watchedApplied=false; }
    if (apply && !watchedApplied) watchedApplied=watchedManager->ApplyStereo();
    if (!apply && watchedApplied) { watchedManager->Disable(); watchedApplied=false; }
}

int main()
{
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::setvbuf(stderr,nullptr,_IONBF,0);
    std::puts("Starting D3D11 VRS validation");
    try {
        ComPtr<IDXGIFactory1> factory;
        HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> next;
            if (factory->EnumAdapters1(i,&next)==DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{}; HR(next->GetDesc1(&desc));
            if (desc.VendorId==0x10de) { adapter=next; std::printf("GPU: %ls\n",desc.Description); break; }
        }
        Require(adapter != nullptr, "NVIDIA GPU required for this hardware test");
        ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
        HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
        ComPtr<ID3D11InfoQueue> debugQueue; HR(dev.As(&debugQueue));
        std::puts("D3D11 device created");
        // A shader created before capture must never be guessed compatible.
        ComPtr<ID3DBlob> legacyBlob;
        const char* legacyCode="float4 main():SV_Target {return float4(0.321,0,0,1);}";
        HR(D3DCompile(legacyCode,std::strlen(legacyCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&legacyBlob,nullptr));
        ComPtr<ID3D11PixelShader> legacyShader;
        HR(dev->CreatePixelShader(legacyBlob->GetBufferPointer(),legacyBlob->GetBufferSize(),nullptr,&legacyShader));
        Require(ocu_vrs_guard::ShaderReasons(legacyShader.Get())==ocu_vrs_guard::Unclassified,"pre-capture shader must remain unclassified");
        Require(ocu_vrs_guard::InstallShaderCapture(dev.Get()),"early shader capture install");
        Require(ocu_vrs_guard::IsNvidiaDevice(dev.Get()),"NVIDIA adapter predicate");
        // Emulate shaders loaded on a game device after temporary-device capture.
        ComPtr<ID3D11Device> nextDevice; ComPtr<ID3D11DeviceContext> nextContext;
        HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&nextDevice,nullptr,&nextContext));
        ComPtr<ID3D11PixelShader> nextShader;
        HR(nextDevice->CreatePixelShader(legacyBlob->GetBufferPointer(),legacyBlob->GetBufferSize(),nullptr,&nextShader));
        Require(ocu_vrs_guard::ShaderReasons(nextShader.Get())==ocu_vrs_guard::Compatible,"early capture tags shaders on subsequent game device");
        Require(ocu_vrs_guard::ShaderReasons(legacyShader.Get())==ocu_vrs_guard::Unclassified,"metadata is scoped to shader object, not reused bytecode or pointer guesses");
        std::puts("Early shader capture across device creation: PASS");
        UINT w=256,h=128;
        auto scene=Color(dev.Get(),w,h);
        auto submitted=Color(dev.Get(),w*2,h*2);
        auto sameSizePost=Color(dev.Get(),w,h);
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width=w; dd.Height=h; dd.MipLevels=1; dd.ArraySize=1;
        dd.Format=DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count=1; dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth,shadow;
        ComPtr<ID3D11DepthStencilView> dsv,shadowView;
        HR(dev->CreateTexture2D(&dd,nullptr,&depth)); HR(dev->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
        HR(dev->CreateTexture2D(&dd,nullptr,&shadow)); HR(dev->CreateDepthStencilView(shadow.Get(),nullptr,&shadowView));
        ocu_vrs_scope::SceneScope scope;
        scope.Arm(ctx.Get(),depth.Get(),submitted.texture.Get(),w,h);
        auto* rtv=scene.view.Get();
        Require(scope.Matches(ctx.Get(),1,&rtv,dsv.Get()), "internal scene color must match main depth despite different submitted output");
        ComPtr<ID3D11RenderTargetView> alternate;
        HR(dev->CreateRenderTargetView(scene.texture.Get(),nullptr,&alternate));
        rtv=alternate.Get();
        Require(scope.Matches(ctx.Get(),1,&rtv,dsv.Get()), "alternate view of same scene texture must remain foveated");
        Require(!scope.Matches(ctx.Get(),1,&rtv,shadowView.Get()), "same-size shadow depth must not qualify");
        Require(!scope.Matches(ctx.Get(),1,&rtv,nullptr), "post-processing without main depth must remain full rate");
        Require(!scope.Matches(nullptr,1,&rtv,dsv.Get()), "foreign context must not mutate VRS");
        rtv=submitted.view.Get();
        Require(!scope.Matches(ctx.Get(),1,&rtv,dsv.Get()), "different resolution target must not receive scene atlas");
        rtv=scene.view.Get();
        ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
            nullptr,nullptr,1,0,nullptr,nullptr);
        bool keepMatched=false;
        ocu_vrs_scope::WithRenderTargets(ctx.Get(),D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,nullptr,nullptr,
            [&](UINT count,ID3D11RenderTargetView* const* views,ID3D11DepthStencilView* currentDepth) {
                keepMatched=scope.Matches(ctx.Get(),count,views,currentDepth);
            });
        Require(keepMatched,"UAV-only KEEP update must preserve active scene classification");
        scope.Reset();
        Require(!scope.Matches(ctx.Get(),1,&rtv,dsv.Get()),"disarmed scope must not match");
        scope.Arm(ctx.Get(),nullptr,scene.texture.Get(),w,h);
        rtv=alternate.Get();
        Require(scope.Matches(ctx.Get(),1,&rtv,nullptr),"fallback supports alternate RTV identity");
        rtv=sameSizePost.view.Get();
        Require(!scope.Matches(ctx.Get(),1,&rtv,nullptr),"fallback must reject unrelated same-size resource");

        VRSManager::EyeRegion original[2]={{0,0,4224,4608},{4224,0,4224,4608}};
        auto left=ocu_vrs_scope::ScaleRegion(original[0],8448,4608,5632,3072);
        auto right=ocu_vrs_scope::ScaleRegion(original[1],8448,4608,5632,3072);
        Require(left.width==2816 && right.left==2816 && right.width==2816 && right.height==3072,
            "Galaxy render-scale transition must retain non-overlapping stereo regions");
        std::puts("Scene resource/view/KEEP/scale checks: PASS");

        const char* vsCode="float4 main(uint id:SV_VertexID):SV_Position { float2 uv=float2((id<<1)&2,id&2); return float4(uv*float2(2,-2)+float2(-1,1),0.5,1); }";
        const char* psCode="float4 main(float4 p:SV_Position):SV_Target { return float4(p.xy,0.25,1); }";
        ComPtr<ID3DBlob> vsBlob,psBlob,error;
        HR(D3DCompile(vsCode,std::strlen(vsCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vsBlob,&error));
        HR(D3DCompile(psCode,std::strlen(psCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&psBlob,&error));
        ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
        HR(dev->CreateVertexShader(vsBlob->GetBufferPointer(),vsBlob->GetBufferSize(),nullptr,&vs));
        HR(dev->CreatePixelShader(psBlob->GetBufferPointer(),psBlob->GetBufferSize(),nullptr,&ps));
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode=D3D11_FILL_SOLID; rd.CullMode=D3D11_CULL_NONE; rd.DepthClipEnable=TRUE;
        ComPtr<ID3D11RasterizerState> rs; HR(dev->CreateRasterizerState(&rd,&rs)); ctx->RSSetState(rs.Get());
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable=TRUE; ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc=D3D11_COMPARISON_ALWAYS;
        ComPtr<ID3D11DepthStencilState> depthState; HR(dev->CreateDepthStencilState(&ds,&depthState));
        ctx->OMSetDepthStencilState(depthState.Get(),0);
        rtv=scene.view.Get(); ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(),nullptr,0); ctx->PSSetShader(ps.Get(),nullptr,0);
        VRSManager manager; Require(manager.Initialize(dev.Get()),"NVAPI VRS initialization required");
        manager.SetProjectionCenters(0.5f,0.5f,0.5f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"production stereo pattern creation");
        auto draw=[&](int eye) {
            D3D11_VIEWPORT vp{float(eye*w/2),0,float(w/2),float(h),0,1}; ctx->RSSetViewports(1,&vp);
            D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0}; ComPtr<ID3D11Query> q;
            HR(dev->CreateQuery(&qd,&q)); ctx->Begin(q.Get()); ctx->Draw(3,0); ctx->End(q.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
            HRESULT result=S_FALSE;
            for(int i=0;i<1000 && result==S_FALSE;++i) { result=ctx->GetData(q.Get(),&stats,sizeof(stats),0); if(result==S_FALSE) Sleep(1); }
            Require(result==S_OK,"GPU query did not complete"); return stats.PSInvocations;
        };
        auto inspectPixels=[&](const char* label) {
            D3D11_TEXTURE2D_DESC desc{}; scene.texture->GetDesc(&desc);
            desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback; HR(dev->CreateTexture2D(&desc,nullptr,&readback));
            ctx->CopyResource(readback.Get(),scene.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{}; HR(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map));
            const float* row=static_cast<const float*>(map.pData);
            std::printf("%s pixels L x=0/1/2: %.1f/%.1f/%.1f; R x=128/129/130: %.1f/%.1f/%.1f\n",
                label,row[0],row[4],row[8],row[128*4],row[129*4],row[130*4]);
            const bool coarseBoth=row[0]==row[4] && row[128*4]==row[129*4];
            ctx->Unmap(readback.Get(),0);
            return coarseBoth;
        };
        auto pairIsFullRate=[&](UINT x, UINT y) {
            D3D11_TEXTURE2D_DESC desc{}; scene.texture->GetDesc(&desc);
            desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback; HR(dev->CreateTexture2D(&desc,nullptr,&readback));
            ctx->CopyResource(readback.Get(),scene.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{}; HR(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map));
            const float* row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
            const bool full=row[x*4]!=row[(x+1)*4];
            ctx->Unmap(readback.Get(),0);
            return full;
        };
        manager.Disable(); const auto fullL=draw(0),fullR=draw(1);
        inspectPixels("full");
        Require(manager.ApplyStereo(),"production VRS apply"); const auto coarseL=draw(0),coarseR=draw(1);
        const bool coarsePixels=inspectPixels("foveated");
        Require(pairIsFullRate(64,64) && pairIsFullRate(192,64),"both gaze centers must retain full-rate detail");
        Require(!pairIsFullRate(32,64) && !pairIsFullRate(224,64),"future gaze centers start in coarse region");
        manager.Disable(); const auto restoredL=draw(0),restoredR=draw(1);
        std::printf("PS invocations L/R: full=%llu/%llu foveated=%llu/%llu restored=%llu/%llu\n",
            fullL,fullR,coarseL,coarseR,restoredL,restoredR);
        std::printf("Both-eye coarse pixel evidence=%d\n",int(coarsePixels));
        Require(fullL>0 && fullR>0 && coarseL<fullL*0.8 && coarseR<fullR*0.8,"both eyes must shade fewer pixels");
        Require(restoredL==fullL && restoredR==fullR,"disabling VRS must restore both eyes to full rate");

        manager.SetProjectionCenters(0.25f,0.5f,0.75f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"moving gaze update");
        Require(manager.ApplyStereo(),"moving gaze apply"); draw(0); draw(1);
        Require(pairIsFullRate(32,64) && pairIsFullRate(224,64),"full-rate detail must follow each eye independently");
        Require(!pairIsFullRate(64,64) && !pairIsFullRate(192,64),"previous gaze centers must become coarse");
        manager.Disable();
        std::puts("Moving gaze preserves detail and updates both eyes: PASS");

        manager.SetProjectionCenters(0.5f,0.5f,0.5f,0.5f);
        watchedManager=&manager; watchedScope=&scope;
        Require(ocu_vrs_guard::ShaderReasons(ps.Get())==ocu_vrs_guard::Compatible,"opaque shader tagged at creation");

        const char* alphaCode = "float4 main(float4 p:SV_Position):SV_Target { uint2 a=uint2(p.xy); uint index=((a.x<<2)&12)|(a.y&3); const float m[16]={0.003922,0.533333,0.133333,0.666667,0.800000,0.266667,0.933333,0.400000,0.200000,0.733333,0.066667,0.600000,0.996078,0.466667,0.866667,0.333333}; clip(0.5-m[index]); return float4(0,1,0,1); }";
        ComPtr<ID3DBlob> alphaBlob;
        HR(D3DCompile(alphaCode,std::strlen(alphaCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&alphaBlob,&error));
        ComPtr<ID3D11PixelShader> alphaPS;
        HR(dev->CreatePixelShader(alphaBlob->GetBufferPointer(),alphaBlob->GetBufferSize(),nullptr,&alphaPS));
        // The depth-only pass uses identical discard arithmetic but no color
        // output; a color-output PS with no RTV would create expected debug
        // warnings and hide unrelated state errors in this fixture.
        std::string alphaDepthCode(alphaCode);
        alphaDepthCode.replace(0,alphaDepthCode.find('{'),"void main(float4 p:SV_Position) ");
        alphaDepthCode.replace(alphaDepthCode.find("return float4(0,1,0,1);"),std::strlen("return float4(0,1,0,1);"),"");
        ComPtr<ID3DBlob> alphaDepthBlob; ComPtr<ID3D11PixelShader> alphaDepthPS;
        HR(D3DCompile(alphaDepthCode.data(),alphaDepthCode.size(),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&alphaDepthBlob,&error));
        HR(dev->CreatePixelShader(alphaDepthBlob->GetBufferPointer(),alphaDepthBlob->GetBufferSize(),nullptr,&alphaDepthPS));
        Require((ocu_vrs_guard::ShaderReasons(alphaPS.Get()) & ocu_vrs_guard::Discard)!=0,"dither shader classified from actual bytecode");
        ComPtr<ID3DBlob> stripped;
        HR(D3DStripShader(alphaBlob->GetBufferPointer(),alphaBlob->GetBufferSize(),D3DCOMPILER_STRIP_REFLECTION_DATA|D3DCOMPILER_STRIP_DEBUG_INFO,&stripped));
        Require((ocu_vrs_guard::ClassifyBytecode(stripped->GetBufferPointer(),stripped->GetBufferSize()) & ocu_vrs_guard::Discard)!=0,"stripped shader retains discard classification");
        const char* specialShaders[]={
            "void main(float4 p:SV_Position,out float4 c:SV_Target,out float d:SV_Depth) {c=1; d=p.z;}",
            "void main(float4 p:SV_Position,out float4 c:SV_Target,out uint m:SV_Coverage) {c=1;m=(uint)p.x;}",
            "RWByteAddressBuffer b:register(u1); float4 main(float4 p:SV_Position):SV_Target {b.Store(0,(uint)p.x);return 1;}"
        };
        const unsigned expectedReasons[]={ocu_vrs_guard::DepthOrCoverage,ocu_vrs_guard::DepthOrCoverage,ocu_vrs_guard::UnorderedAccess};
        for(unsigned i=0;i<3;++i) {
            ComPtr<ID3DBlob> blob;
            HR(D3DCompile(specialShaders[i],std::strlen(specialShaders[i]),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&blob,nullptr));
            Require((ocu_vrs_guard::ClassifyBytecode(blob->GetBufferPointer(),blob->GetBufferSize())&expectedReasons[i])!=0,"depth/coverage/UAV shader protection");
        }
        Require(ocu_vrs_guard::ClassifyBytecode(nullptr,0)==ocu_vrs_guard::Unclassified,"missing bytecode is not opaque");
        std::puts("Bytecode classification: opaque, dither, stripped dither, depth, coverage, UAV PASS");
        ds.DepthEnable=TRUE; ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc=D3D11_COMPARISON_LESS;
        ComPtr<ID3D11DepthStencilState> preDepth,litDepth;
        HR(dev->CreateDepthStencilState(&ds,&preDepth));
        ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc=D3D11_COMPARISON_EQUAL;
        HR(dev->CreateDepthStencilState(&ds,&litDepth));
        scope.Arm(ctx.Get(),depth.Get(),scene.texture.Get(),w,h);
        rtv=scene.view.Get();
        Require(!scope.Matches(ctx.Get(),0,nullptr,dsv.Get()),"Production scope rejects depth-only pass");
        Require(scope.Matches(ctx.Get(),1,&rtv,dsv.Get()),"Production scope accepts paired color pass");
        auto coverage=[&]() {
            D3D11_TEXTURE2D_DESC desc{}; scene.texture->GetDesc(&desc);
            desc.BindFlags=0; desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback; HR(dev->CreateTexture2D(&desc,nullptr,&readback));
            ctx->CopyResource(readback.Get(),scene.texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{}; HR(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&map));
            UINT covered=0,missing=0,extra=0;
            for(UINT y=0;y<h;++y) {
                const float* row=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+y*map.RowPitch);
                for(UINT x=0;x<w;++x) {
                    const float mask[16]={0.003922f,0.533333f,0.133333f,0.666667f,0.800000f,0.266667f,0.933333f,0.400000f,0.200000f,0.733333f,0.066667f,0.600000f,0.996078f,0.466667f,0.866667f,0.333333f};
                    const bool expected=mask[((x<<2)&12)|(y&3)]<=0.5f;
                    const bool green=row[x*4+1]>0.5f;
                    if(green) ++covered;
                    if(expected && !green) ++missing;
                    if(!expected && green) ++extra;
                }
            }
            ctx->Unmap(readback.Get(),0);
            return std::array<UINT,3>{covered,missing,extra};
        };
        auto runAlpha=[&](ocu_foveation::Rate rate, bool protectColor) {
            manager.Disable();
            const float blue[4]={0,0,1,1};
            ctx->ClearRenderTargetView(rtv,blue);
            ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
            ctx->PSSetShader(alphaDepthPS.Get(),nullptr,0);
            ctx->OMSetDepthStencilState(preDepth.Get(),0);
            ctx->OMSetRenderTargets(0,nullptr,dsv.Get());
            draw(0); draw(1);
            ctx->OMSetDepthStencilState(litDepth.Get(),0);
            ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
            ctx->PSSetShader(alphaPS.Get(),nullptr,0);
            Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
                {ocu_foveation::Rate::X1x1,rate,rate}),"alpha fixture rate update");
            if(protectColor) {
                watchedApplied=false;
                Require(ocu_vrs_guard::WatchContext(ctx.Get(),&Changed),"production state hooks installed");
                Changed(ctx.Get(),true);
                Require(!watchedApplied,"discard color shader must automatically remain full rate");
            } else Require(manager.ApplyStereo(),"unprotected audit control VRS apply");
            draw(0); draw(1);
            ocu_vrs_guard::UnwatchContext(ctx.Get()); manager.Disable(); watchedApplied=false;
            const auto counts=coverage();
            std::printf("DITHER rate=%s protectColor=%d covered=%u missingExpected=%u extra=%u\n",ocu_foveation::RateName(rate),int(protectColor),counts[0],counts[1],counts[2]);
            return counts;
        };
        auto baseline=runAlpha(ocu_foveation::Rate::X1x1,false);
        Require(baseline[1]==0 && baseline[2]==0,"full rate alpha reference must match expected coverage");
        auto halfX=runAlpha(ocu_foveation::Rate::X2x1,false);
        auto halfY=runAlpha(ocu_foveation::Rate::X1x2,false);
        auto quarter=runAlpha(ocu_foveation::Rate::X2x2,false);
        auto protectedColor=runAlpha(ocu_foveation::Rate::X2x1,true);
        Require(protectedColor[1]==0 && protectedColor[2]==0,"automatic color protection must restore alpha coverage");
        const auto protectedY=runAlpha(ocu_foveation::Rate::X1x2,true);
        Require(protectedY[1]==0 && protectedY[2]==0,"automatic vertical half-rate protection must restore coverage");
        Require(halfX[1]>0 && halfY[1]>0,"unprotected controls must still reproduce original defect");
        std::printf("DITHER_MISMATCH_REPRO halfX=%d halfY=%d quarter=%d\n",int(halfX[1]>0),int(halfY[1]>0),int(quarter[1]>0));
        // Shader switches without rebinding render targets must recover VRS
        // before the very next ordinary draw, even at depth-equal state.
        ctx->OMSetDepthStencilState(depthState.Get(),0);
        ctx->PSSetShader(ps.Get(),nullptr,0);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"opaque recovery pattern");
        Require(ocu_vrs_guard::WatchContext(ctx.Get(),&Changed),"restart watching"); Changed(ctx.Get(),true);
        Require(draw(0)<fullL*0.8 && draw(1)<fullR*0.8,"opaque draws retain VRS after protection");
        ctx->PSSetShader(alphaPS.Get(),nullptr,0);
        Require(!watchedApplied,"shader-only switch disables coarse coverage");
        ctx->PSSetShader(ps.Get(),nullptr,0);
        Require(watchedApplied && draw(0)<fullL*0.8,"shader-only switch immediately restores VRS");
        ctx->PSSetShader(legacyShader.Get(),nullptr,0);
        Require(!watchedApplied && draw(0)==fullL,"untagged shader runs full rate");
        ctx->PSSetShader(ps.Get(),nullptr,0);
        Require(watchedApplied && draw(1)<fullR*0.8,"unknown shader does not stickily disable foveation");
        D3D11_BLEND_DESC bd{}; bd.AlphaToCoverageEnable=TRUE; bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> atoc; HR(dev->CreateBlendState(&bd,&atoc));
        ctx->OMSetBlendState(atoc.Get(),nullptr,~0u);
        Require(!watchedApplied,"alpha-to-coverage state protected");
        ctx->OMSetBlendState(nullptr,nullptr,~0u);
        Require(watchedApplied,"blend-only switch restores VRS");

        // A dynamic viewport can change without a shader/RTV bind. The cached
        // observation must move both gaze centers before the next draw, keep
        // the allocation-sized resource, and immediately recover from ambiguity.
        const auto resourceBefore = manager.GetPatternUpdates();
        const D3D11_VIEWPORT sharedSubrect{16, 16, 192, 96, 0, 1};
        ctx->RSSetViewports(1, &sharedSubrect);
        Require(watchedApplied, "recognized reduced SBS subrect remains foveated");
        ctx->Draw(3,0);
        Require(pairIsFullRate(64,64) && pairIsFullRate(160,64), "active subrect moves right-eye center from x192 to x160");
        Require(!pairIsFullRate(192,64), "old right-eye gaze center now lies in the peripheral ring");
        const auto unchangedBefore = manager.GetPatternUpdates();
        for (unsigned i=0; i<100; ++i) ctx->RSSetViewports(1, &sharedSubrect);
        Require(manager.GetPatternUpdates().uploads == unchangedBefore.uploads,
            "identical viewport binds must not re-upload rate patterns");
        const D3D11_VIEWPORT ambiguous{0,0,128,64,0,1};
        ctx->RSSetViewports(1, &ambiguous);
        Require(!watchedApplied && !manager.ApplyStereo(), "ambiguous single-eye/small-SBS subrect withholds coarse shading");
        ctx->Draw(3,0);
        Require(pairIsFullRate(16,16), "ambiguous viewport really renders full rate");
        ctx->RSSetViewports(1, &sharedSubrect);
        Require(watchedApplied, "valid viewport-only change immediately restores coarse shading");
        ctx->Draw(3,0);
        Require(!pairIsFullRate(192,64), "recovery is visible in actual GPU pixels");

        // Two viewports are mapped by their bounds, not array order. Distinct
        // eye gaze centers make a reversed interpretation visible in readback.
        manager.SetProjectionCenters(0.25f,0.5f,0.75f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"independent-eye viewport pattern");
        const D3D11_VIEWPORT reversed[2]{{144,16,96,96,0,1},{16,16,96,96,0,1}};
        ctx->RSSetViewports(2,reversed);
        Require(watchedApplied,"reversed viewport-array order maps by eye bounds");
        // This VS selects slot zero; draw it first, then reverse the array to
        // draw the other eye without changing either eye's mapped rectangle.
        ctx->Draw(3,0);
        const D3D11_VIEWPORT forward[2]{reversed[1],reversed[0]};
        ctx->RSSetViewports(2,forward); ctx->Draw(3,0);
        Require(pairIsFullRate(40,64) && pairIsFullRate(216,64),"both viewport-local gaze centers retain full detail");
        Require(!pairIsFullRate(80,64) && !pairIsFullRate(176,64),"opposite-eye gaze must not be applied to either viewport");
        const D3D11_VIEWPORT crossed[2]{{64,0,128,128,0,1},{128,0,128,128,0,1}};
        ctx->RSSetViewports(2,crossed);
        Require(!watchedApplied,"overlapping/ambiguous eye viewports remain full rate");
        ctx->RSSetViewports(2,forward);
        Require(watchedApplied,"two-eye viewport mapping recovers without shader or target change");
        Require(manager.GetPatternUpdates().resourceCreations == resourceBefore.resourceCreations,
            "active viewport changes must reuse the allocation-sized resource");
        manager.SetProjectionCenters(0.5f,0.5f,0.5f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"restore baseline eye centers");
        Require(draw(0)<fullL*0.8 && draw(1)<fullR*0.8,"exact per-eye viewport draws restore baseline stereo mapping");
        std::puts("Active viewport offsets, independent eyes, ambiguity recovery, resource reuse: PASS");
        ctx->PSSetShader(nullptr,nullptr,0);
        Require(!watchedApplied,"null shader clears eligibility");
        ctx->PSSetShader(ps.Get(),nullptr,0);

        ComPtr<ID3D11DeviceContext> deferred; HR(dev->CreateDeferredContext(0,&deferred));
        deferred->RSSetState(rs.Get()); deferred->OMSetDepthStencilState(depthState.Get(),0);
        deferred->OMSetRenderTargets(1,&rtv,dsv.Get());
        deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        deferred->VSSetShader(vs.Get(),nullptr,0); deferred->PSSetShader(alphaPS.Get(),nullptr,0);
        Require(watchedApplied,"foreign/deferred shader bind must not alter immediate VRS");
        deferred->PSSetShader(ps.Get(),nullptr,0);
        for(int e=0;e<2;++e) {
            D3D11_VIEWPORT vp{float(e*w/2),0,float(w/2),float(h),0,1};
            deferred->RSSetViewports(1,&vp); deferred->Draw(3,0);
        }
        ComPtr<ID3D11CommandList> list; HR(deferred->FinishCommandList(FALSE,&list));
        for(BOOL restore: {TRUE,FALSE}) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0}; ComPtr<ID3D11Query> query;
            HR(dev->CreateQuery(&qd,&query)); ctx->Begin(query.Get());
            ctx->ExecuteCommandList(list.Get(),restore); ctx->End(query.Get());
            D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{}; HRESULT result=S_FALSE;
            for(int i=0;i<1000 && result==S_FALSE;++i) {result=ctx->GetData(query.Get(),&stats,sizeof(stats),0);if(result==S_FALSE)Sleep(1);}
            Require(result==S_OK && stats.PSInvocations==fullL+fullR,"command-list playback is full rate");
            if(restore) Require(watchedApplied && draw(0)<fullL*0.8,"restored state resumes VRS immediately after list");
            else Require(!watchedApplied,"non-restoring list clears eligibility");
        }
        Require(protectedCommands>=2,"command list suspension callback delivered");

        auto restorePipeline=[&]() {
            ctx->RSSetState(rs.Get()); ctx->OMSetDepthStencilState(depthState.Get(),0);
            ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(),nullptr,0); ctx->PSSetShader(ps.Get(),nullptr,0);
        };
        restorePipeline(); Require(draw(0)<fullL*0.8,"VRS recovers after command-list reset");
        ComPtr<ID3D11Device1> device1; ComPtr<ID3D11DeviceContext1> context1;
        if(SUCCEEDED(dev.As(&device1)) && SUCCEEDED(ctx.As(&context1))) {
            auto level=dev->GetFeatureLevel(); ComPtr<ID3DDeviceContextState> fresh,previous;
            HR(device1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),nullptr,&fresh));
            context1->SwapDeviceContextState(fresh.Get(),&previous);
            Require(!watchedApplied,"context-state swap clears shader eligibility");
            context1->SwapDeviceContextState(previous.Get(),nullptr);
            Require(watchedApplied && draw(1)<fullR*0.8,"context-state restore immediately restores VRS");
        }
        ctx->ClearState(); Require(!watchedApplied,"ClearState removes VRS");
        restorePipeline(); Require(draw(1)<fullR*0.8,"VRS resumes after ClearState and new bindings");
        ocu_vrs_guard::UnwatchContext(ctx.Get()); manager.Disable(); watchedApplied=false;
        Require(draw(0)==fullL && draw(1)==fullR,"unwatch and disable leave ordinary full-rate output");
        std::puts("Production shader hooks, immediate recovery, state swaps, command lists: PASS");
        ctx->ClearState(); ctx->Flush();
        UINT debugWarnings=0;
        for(UINT64 i=0;i<debugQueue->GetNumStoredMessages();++i) {
            SIZE_T length=0; HR(debugQueue->GetMessage(i,nullptr,&length));
            std::vector<unsigned char> storage(length);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            HR(debugQueue->GetMessage(i,message,&length));
            if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
                ++debugWarnings; std::printf("D3D11 DEBUG: %s\n",message->pDescription);
            }
        }
        Require(debugWarnings==0,"D3D11 debug layer must remain free of warnings/errors");
        std::puts("D3D11 debug warnings/errors: 0");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
