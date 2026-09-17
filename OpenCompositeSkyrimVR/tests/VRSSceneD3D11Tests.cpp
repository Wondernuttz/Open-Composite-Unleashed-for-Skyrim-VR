#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cstring>
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
        HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
        std::puts("D3D11 device created");
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
        scope.Arm(ctx.Get(),depth.Get(),nullptr,w,h);
        Require(scope.Matches(ctx.Get(),1,&rtv,dsv.Get()), "separate upscaled eye outputs need no shared submitted color to identify scene geometry");
        Require(!scope.Matches(ctx.Get(),1,&rtv,nullptr), "separate-output mode cannot attach scene VRS to post-processing");
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

        // A recreated main-depth target is different scene ownership even when
        // its dimensions and the submitted eye textures have not changed.
        // Model the current SKSE publisher's ~1-second refresh interval at 90Hz
        // without sleeping: each real frame still sees the old bridge pointer.
        // The production scope must refuse that identity; actual PS invocation
        // queries show the resulting coverage lapse rather than trusting rings.
        {
            ComPtr<ID3D11Texture2D> replacementDepth;
            ComPtr<ID3D11DepthStencilView> replacementDsv;
            HR(dev->CreateTexture2D(&dd,nullptr,&replacementDepth));
            HR(dev->CreateDepthStencilView(replacementDepth.Get(),nullptr,&replacementDsv));
            rtv=scene.view.Get();
            ctx->OMSetRenderTargets(1,&rtv,replacementDsv.Get());
            unsigned withheldFrames=0;
            for(unsigned frame=0;frame<90;++frame) {
                manager.Disable();
                scope.Arm(ctx.Get(),depth.Get(),nullptr,w,h);
                const bool admitted=scope.Matches(ctx.Get(),1,&rtv,replacementDsv.Get());
                Require(!admitted,"retired bridge depth must not identify a replacement resource");
                if(admitted) Require(manager.ApplyStereo(),"admitted delayed-refresh VRS apply");
                const auto delayedL=draw(0),delayedR=draw(1);
                Require(delayedL==fullL && delayedR==fullR,
                    "stale bridge depth must demonstrate actual full-rate scene work");
                ++withheldFrames;
            }
            // The next boundary after publication must recover immediately.
            scope.Arm(ctx.Get(),replacementDepth.Get(),nullptr,w,h);
            Require(scope.Matches(ctx.Get(),1,&rtv,replacementDsv.Get()),
                "fresh bridge depth must identify replacement scene immediately");
            Require(manager.ApplyStereo(),"fresh depth publication VRS apply");
            const auto recoveredL=draw(0),recoveredR=draw(1);
            Require(recoveredL==coarseL && recoveredR==coarseR,
                "fresh depth publication must recover original foveated GPU work");
            manager.Disable(); scope.Reset();
            ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
            std::printf("Delayed bridge refresh reproduction: %u simulated 90Hz frames full=%llu/%llu; fresh publication=%llu/%llu PS invocations (no wall-clock delay)\n",
                withheldFrames,fullL,fullR,recoveredL,recoveredR);
        }

        manager.SetProjectionCenters(0.25f,0.5f,0.75f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"moving gaze update");
        Require(manager.ApplyStereo(),"moving gaze apply"); draw(0); draw(1);
        Require(pairIsFullRate(32,64) && pairIsFullRate(224,64),"full-rate detail must follow each eye independently");
        Require(!pairIsFullRate(64,64) && !pairIsFullRate(192,64),"previous gaze centers must become coarse");
        manager.Disable();
        std::puts("Moving gaze preserves detail and updates both eyes: PASS");

        manager.SetProjectionCenters(.5f,.5f,.5f,.5f);
        UINT64 shapeWork[3]{}; unsigned shapeIndex=0;
        for(float width:{.5f,1.f,2.f}) {
            manager.SetHorizontalScale(width);
            Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},.4f,.7f,
                {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"ellipse pattern update");
            Require(manager.ApplyStereo(),"ellipse VRS apply");
            const auto leftWork=draw(0),rightWork=draw(1);
            Require(leftWork==rightWork,"ellipse shading work agrees between symmetric eyes");
            Require(pairIsFullRate(64,64) && pairIsFullRate(192,64),"elliptical center remains full rate");
            shapeWork[shapeIndex++]=leftWork;manager.Disable();
        }
        Require(shapeWork[0]<shapeWork[1] && shapeWork[1]<shapeWork[2],"wider actual VRS rings retain more shading work");
        manager.SetHorizontalScale(1.f);
        std::printf("Ellipse widths .5/1/2 actual scene PS invocations=%llu/%llu/%llu: PASS\n",shapeWork[0],shapeWork[1],shapeWork[2]);

        manager.SetProjectionCenters(0.5f,0.5f,0.5f,0.5f);
        Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
            {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"offset tile fixture pattern");
        const D3D11_VIEWPORT offsetViewport{17,17,192,96,0,1};
        Require(manager.UpdateActiveViewports(1,&offsetViewport) && manager.ApplyStereo(),"non-tile-aligned stereo viewport");
        // Render across the atlas in this direct manager test to expose the
        // rate assigned to padding and the tile that straddles the eye seam.
        draw(0); draw(1);
        Require(pairIsFullRate(16,64) && pairIsFullRate(112,64) && pairIsFullRate(208,64),
            "tiles crossing atlas padding or the moved eye seam must remain full rate");
        Require(pairIsFullRate(64,64) && pairIsFullRate(160,64) && !pairIsFullRate(192,64),
            "unaligned offsets preserve both gaze centers and peripheral reduction");
        const D3D11_VIEWPORT zero{0,0,0,128,0,1};
        Require(!manager.UpdateActiveViewports(1,&zero) && !manager.ApplyStereo(),"invalid viewport prevents stale pattern re-apply");
        Require(draw(0)==fullL && draw(1)==fullR,"invalid viewport actually disables both eyes");
        const D3D11_VIEWPORT fullViewport{0,0,float(w),float(h),0,1};
        Require(manager.UpdateActiveViewports(1,&fullViewport) && manager.ApplyStereo(),"full stereo viewport immediately recovers");
        Require(draw(0)<fullL*0.8 && draw(1)<fullR*0.8,"full stereo recovery affects actual GPU work");
        manager.Disable();
        std::puts("Viewport edge tiles and failed-map recovery: PASS");
        // Large stereo fixtures exercise Dream Air/Air SE panel-sized targets
        // and supersampling. Runtime-recommended targets can differ from panels.
        // Finish at the original size for the subsequent lifecycle assertions.
        const UINT renderSizes[][2]={{7680,3552},{5120,2560},{11520,5328},{192,96},{256,128}};
        for (const auto& size : renderSizes) {
            w=size[0]; h=size[1];
            scene=Color(dev.Get(),w,h);
            dsv.Reset(); depth.Reset(); dd.Width=w; dd.Height=h;
            HR(dev->CreateTexture2D(&dd,nullptr,&depth)); HR(dev->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
            rtv=scene.view.Get(); ctx->OMSetRenderTargets(1,&rtv,dsv.Get());
            const auto resizedFullL=draw(0),resizedFullR=draw(1);
            Require(manager.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
                {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"resize recreates pattern");
            Require(manager.ApplyStereo(),"resized VRS apply");
            const auto resizedCoarseL=draw(0),resizedCoarseR=draw(1);
            manager.Disable();
            Require(resizedCoarseL<resizedFullL*0.8 && resizedCoarseR<resizedFullR*0.8,"resized atlas shades both eyes coarsely on first use");
            Require(draw(0)==resizedFullL && draw(1)==resizedFullR,"resize still restores full shading");
            std::printf("Resize %ux%u full=%llu/%llu foveated=%llu/%llu: PASS\n",w,h,resizedFullL,resizedFullR,resizedCoarseL,resizedCoarseR);
        }
        Require(!manager.UpdateStereoPattern(w,h,{0,0,int(w),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,{}),"overlapping regions remain rejected");
        manager.Shutdown();
        {
            VRSManager nextSession;
            Require(nextSession.Initialize(dev.Get()),"new compositor can reuse process NVAPI initialization");
            Require(nextSession.UpdateStereoPattern(w,h,{0,0,int(w/2),int(h)},{int(w/2),0,int(w/2),int(h)},0.3f,0.6f,
                {ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X2x2}),"new compositor pattern");
            Require(nextSession.ApplyStereo(),"new compositor VRS apply");
            Require(draw(0)<fullL*0.8 && draw(1)<fullR*0.8,"new compositor must shade both eyes coarsely");
        }
        Require(draw(0)==fullL && draw(1)==fullR,"compositor shutdown restores shading while game D3D objects remain live");
        std::puts("Compositor shutdown/recreation with live game resources: PASS");
        Require(coarsePixels,"both eyes must exhibit actual coarse shading");
        std::puts("Production NVAPI stereo rendering: PASS");
        ctx->ClearState(); ctx->Flush();
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
