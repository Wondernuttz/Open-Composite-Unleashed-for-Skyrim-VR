#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include "OpenOVR/Compositor/VRSManager.h"

using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char*) {}
void oovr_log_raw_format(const char*, long, const char*, const char*, ...) {}
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void HR(HRESULT result) { Require(SUCCEEDED(result), "D3D11 call failed"); }

// No production readback or diagnostic collection is needed for these checks.
struct VRSBlackoutTestAccess {
    static void Geometry(VRSManager& manager, int width, int height,
        const std::array<ocu_vrs_scope::ViewportEyeRegion, 2>& eyes, float inner, float middle) {
        manager.patternWidth=(width+15)/16; manager.patternHeight=(height+15)/16;
        manager.renderWidth=width; manager.renderHeight=height;
        manager.activeEyeRegions[0]=eyes[0]; manager.activeEyeRegions[1]=eyes[1];
        manager.cachedInnerRadius=inner; manager.cachedMidRadius=middle;
    }
    static std::vector<uint8_t> Pattern(const VRSManager& manager) { return manager.CreateStereoPattern(); }
    static void Clean(VRSManager& manager) { manager.patternDirty=false; }
    static bool Dirty(const VRSManager& manager) { return manager.patternDirty; }
};

struct Geometry {
    int width, height;
    std::array<ocu_vrs_scope::ViewportEyeRegion, 2> eyes;
    float gaze[2][2]{{.35f,.48f},{.65f,.52f}};
    float inner=.2f, middle=.6f, scale=1.f;
};

static int TileEye(const Geometry& g, int left, int top) {
    for (int eye=0;eye<2;++eye) {
        const auto& r=g.eyes[eye];
        if (left>=r.left && top>=r.top && left+16<=r.left+r.width && top+16<=r.top+r.height)
            return eye;
    }
    return -1;
}

// Frozen ordinary ring selection, independent of the new blackout helper.
static std::vector<uint8_t> OrdinaryPattern(const Geometry& g) {
    const int columns=(g.width+15)/16,rows=(g.height+15)/16;
    std::vector<uint8_t> pattern(columns*rows,0);
    for (int y=0;y<rows;++y) for (int x=0;x<columns;++x) {
        const int eye=TileEye(g,x*16,y*16); if (eye<0) continue;
        const auto& r=g.eyes[eye];
        const float dx=(((x*16+8.f)-r.left)/r.width-g.gaze[eye][0])/g.scale;
        const float dy=((y*16+8.f)-r.top)/r.height-g.gaze[eye][1];
        const float distance=2.f*std::sqrt(dx*dx+dy*dy);
        pattern[y*columns+x]=distance<g.inner?1:distance<g.middle?2:3;
    }
    return pattern;
}

static bool HiddenPixel(const ocu_foveation::Blackout& mask, const Geometry& g,
    int eye, double x, double y) {
    const auto& r=g.eyes[eye];
    const double dx=2*((x-r.left)/r.width-g.gaze[eye][0])/g.scale;
    const double dy=2*((y-r.top)/r.height-g.gaze[eye][1]);
    const double distance=std::hypot(dx,dy);
    const double cutoff=std::max(double(g.middle),double(mask.cutoffRadius));
    return (mask.middle && distance>g.inner && distance<=g.middle) ||
        (mask.outer && distance>g.middle) || (mask.cutoff && distance>cutoff);
}

static void SetGeometry(VRSManager& manager,const Geometry& g) {
    VRSBlackoutTestAccess::Geometry(manager,g.width,g.height,g.eyes,g.inner,g.middle);
    manager.SetProjectionCenters(g.gaze[0][0],g.gaze[0][1],g.gaze[1][0],g.gaze[1][1]);
    manager.SetHorizontalScale(g.scale);
}

static void CPUChecks() {
    const Geometry layouts[]={
        {1024,512,{{{0,0,512,512},{512,0,512,512}}}},
        {512,1024,{{{0,0,512,512},{0,512,512,512}}}},
        {1031,529,{{{7,9,499,501},{521,17,497,503}}}}
    };
    size_t checkedTiles=0,checkedSamples=0;
    for (auto g:layouts) for (float scale:{.5f,1.f,2.f}) {
        g.scale=scale; VRSManager manager; SetGeometry(manager,g);
        const auto expected=OrdinaryPattern(g);
        Require(VRSBlackoutTestAccess::Pattern(manager)==expected,"disabled blackout changed ordinary pattern bytes");
        VRSBlackoutTestAccess::Clean(manager);
        ocu_foveation::Blackout inactive;inactive.cutoffRadius=.7f;inactive.guardPixels=4;
        manager.SetBlackout(inactive);
        Require(!VRSBlackoutTestAccess::Dirty(manager),"inactive optional values dirtied ordinary VRS");
        for (unsigned flags=1;flags<8;++flags) {
            ocu_foveation::Blackout mask;
            mask.middle=(flags&1)!=0;mask.outer=(flags&2)!=0;mask.cutoff=(flags&4)!=0;
            mask.cutoffRadius=.8f;mask.guardPixels=4;
            manager.SetBlackout(mask);
            const auto pattern=VRSBlackoutTestAccess::Pattern(manager);
            const int columns=(g.width+15)/16;
            for (size_t i=0;i<pattern.size();++i) {
                ++checkedTiles;
                const int left=int(i%columns)*16,top=int(i/columns)*16;
                const int eye=TileEye(g,left,top);
                if (eye<0) { Require(pattern[i]==0,"Cull/coarse tile crossed eye seam or padding");continue; }
                if (pattern[i]!=4) { Require(pattern[i]==expected[i],"blackout changed an ordinary rate index");continue; }
                // Dense independent samples include tile edges and the full guard.
                for (double y=top-mask.guardPixels;y<=top+16+mask.guardPixels;y+=1.)
                    for (double x=left-mask.guardPixels;x<=left+16+mask.guardPixels;x+=1.) {
                        Require(HiddenPixel(mask,g,eye,x,y),"Cull tile or guard intersects visible view");
                        ++checkedSamples;
                    }
            }
            VRSBlackoutTestAccess::Clean(manager);manager.SetBlackout(mask);
            Require(!VRSBlackoutTestAccess::Dirty(manager),"unchanged active profile dirtied VRS");
        }
        manager.SetBlackout({});
        Require(VRSBlackoutTestAccess::Dirty(manager),"removing blackout failed to dirty replacement pattern");
        Require(VRSBlackoutTestAccess::Pattern(manager)==expected,"removing blackout did not restore ordinary pattern bytes");

        ocu_foveation::Blackout below;below.cutoff=true;below.cutoffRadius=.1f;below.guardPixels=4;
        manager.SetBlackout(below);const auto belowPattern=VRSBlackoutTestAccess::Pattern(manager);
        below.cutoffRadius=g.middle;manager.SetBlackout(below);
        Require(VRSBlackoutTestAccess::Pattern(manager)==belowPattern,"cutoff below middle was not canonicalized");
        below.guardPixels=-1;manager.SetBlackout(below);
        Require(VRSBlackoutTestAccess::Pattern(manager)==expected,"invalid negative guard enabled Cull");
        below.guardPixels=std::numeric_limits<float>::quiet_NaN();manager.SetBlackout(below);
        Require(VRSBlackoutTestAccess::Pattern(manager)==expected,"invalid NaN guard enabled Cull");
        below.guardPixels=4;manager.SetBlackout(below);manager.SetProjectionCenters(-.1f,.5f,1.1f,.5f);
        const auto invalidGaze=VRSBlackoutTestAccess::Pattern(manager);
        Require(std::find(invalidGaze.begin(),invalidGaze.end(),uint8_t(4))==invalidGaze.end(),"invalid gaze enabled Cull");
    }
    Require(checkedSamples>10000,"CPU coverage fixture exercised too few hidden samples");
    std::printf("CPU: ordinary bytes, 7 blackout combinations, widths, horizontal/vertical/unaligned eyes, guards, cutoff normalization, disable; tiles=%zu samples=%zu PASS\n",checkedTiles,checkedSamples);
}

struct Pixel { float r,g,b,a; };
struct Capture { UINT64 invocations=0;std::vector<Pixel> pixels; };

struct GPUFixture {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vertex;ComPtr<ID3D11PixelShader> pixel;
    VRSManager manager;
    Geometry geometry{1024,512,{{{0,0,512,512},{512,0,512,512}}}};
    GPUFixture() {
        ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> next;if(factory->EnumAdapters1(i,&next)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};HR(next->GetDesc1(&desc));
            if(desc.VendorId==0x10de){adapter=next;std::printf("GPU: %ls\n",desc.Description);break;}
        }
        Require(adapter!=nullptr,"NVIDIA VRS hardware required");
        HR(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=geometry.width;desc.Height=geometry.height;
        desc.MipLevels=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        HR(device->CreateTexture2D(&desc,nullptr,&target));HR(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc,nullptr,&staging));
        const char* source=R"(
float4 vs(uint id:SV_VertexID):SV_Position { float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),.5,1); }
float4 ps(float4 p:SV_Position):SV_Target { uint2 q=uint2(p.xy);return float4(frac(p.x/127),frac(p.y/113),(q.x/7+q.y/11)&1,1); }
)";
        ComPtr<ID3DBlob> vs,ps,error;
        HR(D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error));
        HR(D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"ps","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error));
        HR(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&vertex));
        HR(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&pixel));
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        ComPtr<ID3D11RasterizerState> rs;HR(device->CreateRasterizerState(&rd,&rs));context->RSSetState(rs.Get());
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;ComPtr<ID3D11DepthStencilState> ds;
        HR(device->CreateDepthStencilState(&dd,&ds));context->OMSetDepthStencilState(ds.Get(),0);
        Require(manager.Initialize(device.Get()),"native VRS unavailable");Update({});
    }
    void Update(const ocu_foveation::Blackout& mask) {
        manager.SetProjectionCenters(geometry.gaze[0][0],geometry.gaze[0][1],geometry.gaze[1][0],geometry.gaze[1][1]);
        manager.SetHorizontalScale(geometry.scale);manager.SetBlackout(mask);
        Require(manager.UpdateStereoPattern(geometry.width,geometry.height,{0,0,512,512},{512,0,512,512},
            geometry.inner,geometry.middle,{ocu_foveation::Rate::X1x1,ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X4x2}),"VRS pattern update failed");
    }
    Capture Draw(bool apply=true) {
        if(apply)Require(manager.ApplyStereo(),"VRS apply failed");
        const float poison[]={1000,0,0,1};context->ClearRenderTargetView(rtv.Get(),poison);
        auto* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(),nullptr,0);context->PSSetShader(pixel.Get(),nullptr,0);
        D3D11_VIEWPORT vp{0,0,float(geometry.width),float(geometry.height),0,1};context->RSSetViewports(1,&vp);
        D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS,0};ComPtr<ID3D11Query> query;
        HR(device->CreateQuery(&qd,&query));context->Begin(query.Get());context->Draw(3,0);context->End(query.Get());context->Flush();
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};HRESULT result=S_FALSE;
        for(unsigned i=0;i<10000 && result==S_FALSE;++i) {
            result=context->GetData(query.Get(),&stats,sizeof(stats),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(result==S_FALSE)Sleep(1);
        }
        Require(result==S_OK,"GPU query timeout");
        context->OMSetRenderTargets(0,nullptr,nullptr);context->CopyResource(staging.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        Capture capture;capture.invocations=stats.PSInvocations;capture.pixels.resize(size_t(geometry.width)*geometry.height);
        for(int y=0;y<geometry.height;++y)std::memcpy(capture.pixels.data()+size_t(y)*geometry.width,
            static_cast<const char*>(mapped.pData)+size_t(y)*mapped.RowPitch,size_t(geometry.width)*sizeof(Pixel));
        context->Unmap(staging.Get(),0);return capture;
    }
    size_t Validate(const Capture& baseline,const Capture& candidate,const ocu_foveation::Blackout& mask) {
        const auto pattern=VRSBlackoutTestAccess::Pattern(manager);const int columns=(geometry.width+15)/16;
        size_t visible=0,poison=0;
        for(int y=0;y<geometry.height;++y)for(int x=0;x<geometry.width;++x) {
            const size_t i=size_t(y)*geometry.width+x;const int eye=x<512?0:1;
            const bool culled=pattern[size_t(y/16)*columns+x/16]==4;
            Require((candidate.pixels[i].r==1000)==culled,"native Cull coverage does not match tile mask");
            if(culled)++poison;
            if(!HiddenPixel(mask,geometry,eye,x+.5,y+.5)) {
                Require(std::memcmp(&candidate.pixels[i],&baseline.pixels[i],sizeof(Pixel))==0,"Cull changed visible spatial pixel");++visible;
            }
        }
        Require(poison>0 && visible>0,"fixture did not exercise hidden and visible regions");
        Require(candidate.invocations<baseline.invocations,"native Cull did not reduce actual pixel shader work");
        std::printf("Native flags=%d%d%d cutoff=%.2f visible=%zu skippedPixels=%zu PS=%llu -> %llu PASS\n",
            int(mask.middle),int(mask.outer),int(mask.cutoff),mask.cutoffRadius,visible,poison,baseline.invocations,candidate.invocations);
        return poison;
    }
};

static void GPUChecks() {
    GPUFixture fixture;const auto baseline=fixture.Draw();
    for(unsigned flags=1;flags<8;++flags) {
        ocu_foveation::Blackout mask;mask.middle=(flags&1)!=0;mask.outer=(flags&2)!=0;mask.cutoff=(flags&4)!=0;
        mask.cutoffRadius=.8f;mask.guardPixels=4;fixture.Update(mask);
        fixture.Validate(baseline,fixture.Draw(),mask);
        const auto uploads=fixture.manager.GetPatternUpdates().uploads;
        fixture.Update(mask);Require(fixture.manager.GetPatternUpdates().uploads==uploads,"unchanged blackout reuploaded pattern");
    }
    ocu_foveation::Blackout below;below.middle=true;below.cutoff=true;below.cutoffRadius=.1f;below.guardPixels=4;
    fixture.Update(below);fixture.Validate(baseline,fixture.Draw(),below);

    // Removal must unbind immediately, even before the replacement upload.
    fixture.manager.SetBlackout({});Require(!fixture.manager.ApplyStereo(),"dirty removal accepted stale pattern");
    const auto unbound=fixture.Draw(false);
    Require(unbound.invocations==UINT64(fixture.geometry.width)*fixture.geometry.height,"removing blackout left native Cull bound");
    fixture.Update({});const auto restored=fixture.Draw();
    Require(restored.invocations==baseline.invocations &&
        std::memcmp(restored.pixels.data(),baseline.pixels.data(),baseline.pixels.size()*sizeof(Pixel))==0,
        "blackout removal did not restore ordinary VRS image/work");

    fixture.geometry.gaze[0][0]=.65f;fixture.geometry.gaze[1][0]=.35f;
    fixture.geometry.scale=.65f;fixture.Update({});const auto moved=fixture.Draw();
    ocu_foveation::Blackout cutoff;cutoff.cutoff=true;cutoff.cutoffRadius=.8f;fixture.Update(cutoff);
    fixture.Validate(moved,fixture.Draw(),cutoff);
    fixture.manager.Disable();const auto full=fixture.Draw(false);
    Require(full.invocations==UINT64(fixture.geometry.width)*fixture.geometry.height,"Disable did not restore full-rate rendering");
    std::puts("Native spatial output, skipped work, moving gaze, immediate unbind, ordinary/full-rate recovery PASS (no timing claims)");
}

int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    try {
        CPUChecks();
        if(argc<2 || std::string(argv[1])!="cpu-only")GPUChecks();
        return 0;
    } catch(const std::exception& error) {
        std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;
    }
}
