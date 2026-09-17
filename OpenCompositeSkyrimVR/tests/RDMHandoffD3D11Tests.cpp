#include <d3d11_1.h>
#include <d3d11_4.h>
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
#ifdef OCU_RDM_TEST_DETOURS
#include <detours/detours.h>
#endif
#include "OpenOVR/Compositor/RDMRenderScope.h"
#include "OpenOVR/Compositor/VRSShaderGuard.h"
#include "OpenOVR/Compositor/ExactPixelShader.h"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*, long, const char*, const char* msg) { std::puts(msg); }
void oovr_log_raw_format(const char*, long, const char*, const char* fmt, ...) {
    va_list args; va_start(args,fmt); std::vprintf(fmt,args); va_end(args); std::puts("");
}
static void Require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
// Manual scene-only timing fixtures bypass the draw detour while busy. Supply
// the same current-geometry coverage proof the production detour requires.
static bool PrepareCoveredDraw(RDMRenderScope& scope, ID3D11DeviceContext* context) {
    if (scope.BeginColorCoverage({}, false)) {
        context->Draw(3, 0);
        scope.EndColorCoverage();
    }
    return scope.BeforeDraw();
}
static void HRAt(HRESULT hr,unsigned line,const char* expression) {
    if(FAILED(hr)) {
        std::printf("HRESULT=%08X at RDMHandoffD3D11Tests.cpp:%u: %s\n",unsigned(hr),line,expression);
        throw std::runtime_error("D3D11 call failed");
    }
}
#define HR(expression) HRAt((expression),__LINE__,#expression)
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
void Observe(ID3D11DeviceContext* c) {
    if(checkDepthGuide) {
        ComPtr<ID3D11DepthStencilView> d;c->OMGetRenderTargets(0,nullptr,&d);
        ComPtr<ID3D11PixelShader> p;c->PSGetShader(&p,nullptr,nullptr);
        Require(d.Get()==expected && !p,"depth-prepass observer saw OCU guide bindings");++acceptedDepth;
    }
    ComPtr<ID3D11VertexShader> bound;c->VSGetShader(&bound,nullptr,nullptr);
    if(check && bound.Get()==expectedShader) { ComPtr<ID3D11DepthStencilView> d;c->OMGetRenderTargets(0,nullptr,&d);
        Require(d.Get()==expected,"CSX post-draw observer saw private depth");++accepted; }
}
void STDMETHODCALLTYPE DrawCall(ID3D11DeviceContext* c,UINT n,UINT first) {
    draw(c,n,first);Observe(c);
}
void STDMETHODCALLTYPE SetTargets(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d) {
    targets(c,n,rt,d);RDMRenderScope::NotifyTargets(c);
}
void STDMETHODCALLTYPE SetTargetsUav(ID3D11DeviceContext* c,UINT n,ID3D11RenderTargetView*const* rt,ID3D11DepthStencilView* d,UINT start,UINT count,ID3D11UnorderedAccessView*const* u,const UINT* initial) {
    targetsUav(c,n,rt,d,start,count,u,initial);RDMRenderScope::NotifyTargets(c);
}
struct Table {
    ID3D11DeviceContext* context; void** original;std::array<void*,134> table;bool wrapDraw,enabled;
    void WrapCurrentTable() {
        if(!enabled)return;
        original=*reinterpret_cast<void***>(context);
        std::copy_n(original,table.size(),table.begin());
        draw=reinterpret_cast<Draw>(table[13]);targets=reinterpret_cast<Targets>(table[33]);targetsUav=reinterpret_cast<TargetsUav>(table[34]);
        if(wrapDraw)table[13]=reinterpret_cast<void*>(&DrawCall);
        table[33]=reinterpret_cast<void*>(&SetTargets);table[34]=reinterpret_cast<void*>(&SetTargetsUav);
        *reinterpret_cast<void***>(context)=table.data();
    }
    Table(ID3D11DeviceContext* c,bool a_wrapDraw=true,bool a_enabled=true):
        context(c),original(nullptr),wrapDraw(a_wrapDraw),enabled(a_enabled) { WrapCurrentTable(); }
    void RestoreNativeTable() { if(enabled)*reinterpret_cast<void***>(context)=original; }
    ~Table() { check=false;RestoreNativeTable(); }
};
}

#ifdef OCU_RDM_TEST_DETOURS
namespace InlineWrapped {
Wrapped::Draw draw=nullptr;
void STDMETHODCALLTYPE DrawCall(ID3D11DeviceContext* c,UINT n,UINT first) {
    draw(c,n,first);Wrapped::Observe(c);
}
struct Hook {
    bool installed=false;
    void Install(ID3D11DeviceContext* context) {
        draw=reinterpret_cast<Wrapped::Draw>((*reinterpret_cast<void***>(context))[13]);
        Require(DetourTransactionBegin()==NO_ERROR,"Detours begin failed");
        Require(DetourUpdateThread(GetCurrentThread())==NO_ERROR,"Detours thread enlistment failed");
        const auto attached=DetourAttach(reinterpret_cast<PVOID*>(&draw),reinterpret_cast<PVOID>(&DrawCall));
        if(attached!=NO_ERROR) {DetourTransactionAbort();Require(false,"Detours inline post-draw attach failed");}
        Require(DetourTransactionCommit()==NO_ERROR,"Detours inline post-draw commit failed");
        installed=true;
    }
    ~Hook() {
        Wrapped::check=false;Wrapped::checkDepthGuide=false;
        if(!installed)return;
        // Keep exception cleanup non-throwing; the process is already exiting
        // after a failed fixture if the deliberate late-order probe fails.
        if(DetourTransactionBegin()!=NO_ERROR)return;
        if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&draw),reinterpret_cast<PVOID>(&DrawCall))!=NO_ERROR) {
            DetourTransactionAbort();return;
        }
        DetourTransactionCommit();
    }
};
}
namespace InlineIndexed {
using Draw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
Draw draw=nullptr;
bool observing=false;
unsigned callbacks=0;
unsigned depthMismatches=0,shaderMismatches=0;
ID3D11PixelShader* expectedShader=nullptr;
void STDMETHODCALLTYPE DrawCall(ID3D11DeviceContext* c,UINT count,UINT first,INT base) {
    if(observing)std::printf("Warm indexed observer: enter count=%u first=%u base=%d trampoline=%p\n",
        count,first,base,reinterpret_cast<void*>(draw));
    draw(c,count,first,base);
    if(observing) {
        std::puts("Warm indexed observer: native/OCU chain returned; inspecting post-draw state");
        ComPtr<ID3D11DepthStencilView> depth;c->OMGetRenderTargets(0,nullptr,&depth);
        ComPtr<ID3D11PixelShader> pixelShader;c->PSGetShader(&pixelShader,nullptr,nullptr);
        // Never throw through a native D3D11 dispatch frame: Windows may
        // fail-fast while unwinding it. Record the exact observer violation
        // here, then fail the fixture after the application's draw returns.
        if(depth.Get()!=Wrapped::expected) {
            ++depthMismatches;
            std::printf("Warm indexed observer DSV MISMATCH: actual=%p expected=%p\n",
                static_cast<void*>(depth.Get()),static_cast<void*>(Wrapped::expected));
        }
        if(pixelShader.Get()!=expectedShader) {
            ++shaderMismatches;
            std::printf("Warm indexed observer PS MISMATCH: actual=%p expected=%p\n",
                static_cast<void*>(pixelShader.Get()),static_cast<void*>(expectedShader));
        }
        ++callbacks;
    }
}
struct Hook {
    bool installed=false;
    void Install(void* warmEntry) {
        std::printf("Warm indexed Detours: attaching target=%p callback=%p\n",
            warmEntry,reinterpret_cast<void*>(&DrawCall));
        draw=reinterpret_cast<Draw>(warmEntry);
        Require(DetourTransactionBegin()==NO_ERROR,"indexed Detours begin failed");
        Require(DetourUpdateThread(GetCurrentThread())==NO_ERROR,"indexed Detours thread enlistment failed");
        if(DetourAttach(reinterpret_cast<PVOID*>(&draw),reinterpret_cast<PVOID>(&DrawCall))!=NO_ERROR) {
            DetourTransactionAbort();Require(false,"warm indexed Detours attach failed");
        }
        Require(DetourTransactionCommit()==NO_ERROR,"warm indexed Detours commit failed");installed=true;
        std::printf("Warm indexed Detours: attached trampoline=%p\n",reinterpret_cast<void*>(draw));
    }
    ~Hook() {
        observing=false;Wrapped::check=false;Wrapped::checkDepthGuide=false;
        if(!installed)return;
        std::puts("Warm indexed Detours: detaching observer");
        if(DetourTransactionBegin()!=NO_ERROR)return;
        if(DetourUpdateThread(GetCurrentThread())!=NO_ERROR ||
            DetourDetach(reinterpret_cast<PVOID*>(&draw),reinterpret_cast<PVOID>(&DrawCall))!=NO_ERROR) {
            DetourTransactionAbort();return;
        }
        const auto committed=DetourTransactionCommit();
        std::printf("Warm indexed Detours: detach commit=%ld\n",long(committed));
    }
};
}
#endif

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
Outputs Seed(float4 p:SV_POSITION) {
    uint2 q=uint2(p.xy);
    Outputs o;
    o.color=float4((q.x%4+1)/8.0,(q.y%4+1)/8.0,((q.x+q.y)%4+1)/8.0,((q.x+2*q.y)%4+1)/8.0);
    o.motion=float4(((q.x+1)%4+1)/8.0,((q.y+1)%4+1)/8.0,((q.x+3*q.y)%4+1)/8.0,((2*q.x+q.y)%4+1)/8.0);
    return o;
}
Outputs Partial(float4 p:SV_POSITION) {
    Outputs o;o.color=float4(.75,.625,.875,1);o.motion=float4(.625,.75,.875,1);return o;
}
float4 PartialOne(float4 p:SV_POSITION):SV_Target0 { return float4(.75,.625,.875,1); }
float4 PartialSecond(float4 p:SV_POSITION):SV_Target1 { return float4(.625,.75,.875,1); }
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
enum class ThreadingMode { Unprotected, Protected, EnableAfterInstall };
enum class InlineHookMode { None, Late, Early };
using NativeIndexedDraw=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
// Read the native dispatch entries without replacing the context's table.
// D3D11 can switch these entries during its first state/draw call even though
// ID3D11Multithread continues to report the same protection mode.
struct NativeDrawDispatch {
    void** table=nullptr;
    std::array<void*,7> entries{};
    explicit NativeDrawDispatch(ID3D11DeviceContext* context) {
        constexpr unsigned slots[]={12,13,20,21,38,39,40};
        table=*reinterpret_cast<void***>(context);
        for(unsigned i=0;i<entries.size();++i)entries[i]=table[slots[i]];
    }
};
static void Run(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver,UINT width,UINT height,bool d24,
    DXGI_FORMAT auxiliaryFormat=DXGI_FORMAT_R8G8B8A8_UNORM,bool separateSubmittedEyes=false,
    float viewportMinDepth=0,float viewportMaxDepth=1,ThreadingMode threading=ThreadingMode::Unprotected,
    InlineHookMode inlineHooks=InlineHookMode::None,bool nativeLazyDispatch=false,bool nativeWarmDetours=false) {
    Require(!nativeWarmDetours || nativeLazyDispatch,"warm native Detours fixture requires native dispatch mode");
    ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    // Match the non-debug FL11.0 game context for the lazy-dispatch probe;
    // a debug context adds wrappers which can conceal this native transition.
    UINT flags=nativeLazyDispatch?0:D3D11_CREATE_DEVICE_DEBUG;
    HRESULT created=D3D11CreateDevice(adapter,driver,nullptr,flags,
        nativeLazyDispatch?levels+1:levels,nativeLazyDispatch?1:2,D3D11_SDK_VERSION,&dev,&level,&ctx);
    if(created==DXGI_ERROR_SDK_COMPONENT_MISSING) { flags=0;created=D3D11CreateDevice(adapter,driver,nullptr,flags,levels,2,D3D11_SDK_VERSION,&dev,&level,&ctx); }
    HR(created);
    ComPtr<ID3D11Multithread> multithread;HR(ctx.As(&multithread));
    BOOL expectedProtection=threading==ThreadingMode::Protected;
    multithread->SetMultithreadProtected(expectedProtection);
    Require(multithread->GetMultithreadProtected()==expectedProtection,"fixture could not set game-context protection");
    NativeIndexedDraw coldIndexed=nullptr;
    if(nativeLazyDispatch) {
        Require(threading==ThreadingMode::Protected && inlineHooks==InlineHookMode::None,
            "native lazy-dispatch fixture requires protected, unwrapped game context");
        const NativeDrawDispatch cold(ctx.Get());
        coldIndexed=reinterpret_cast<NativeIndexedDraw>(cold.entries[0]);
        Require(RDMRenderScope::PrepareDrawHooks(dev.Get()),"native lazy-dispatch bootstrap failed");
        const NativeDrawDispatch bootstrapped(ctx.Get());
        Require(bootstrapped.table==cold.table && bootstrapped.entries==cold.entries,
            "private-probe bootstrap changed the game context's dispatch table");
        Require(RDMRenderScope::Active(ctx.Get())==nullptr,"early bootstrap armed RDM on the game context");
        // A read-only getter initializes native dispatch without submitting an
        // invalid draw on a context that has no shader or pipeline bound yet.
        ComPtr<ID3D11VertexShader> initialShader;ctx->VSGetShader(&initialShader,nullptr,nullptr);
        Require(!initialShader,"fresh native context unexpectedly has a vertex shader");
        const NativeDrawDispatch warm(ctx.Get());
        unsigned changed=0;
        for(unsigned i=0;i<cold.entries.size();++i)if(cold.entries[i]!=warm.entries[i])changed|=1u<<i;
        Require(multithread->GetMultithreadProtected()==TRUE,"native initialization changed game-context protection");
        Require(RDMRenderScope::Active(ctx.Get())==nullptr,"native dispatch initialization activated RDM");
        std::printf("Native lazy dispatch: table=%p -> %p changedDrawMask=0x%02X indexed=%p -> %p draw=%p -> %p protection=1\n",
            static_cast<void*>(cold.table),static_cast<void*>(warm.table),changed,
            cold.entries[0],warm.entries[0],cold.entries[1],warm.entries[1]);
        if(!changed)std::puts("Native dispatch addresses remained stable on this runtime; unwrapped draw coverage is still required.");
    }
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
#ifdef OCU_RDM_TEST_DETOURS
    InlineWrapped::Hook inlineObserver;
    InlineIndexed::Hook indexedObserver;
    if(nativeWarmDetours) {
        // Bootstrap precedes the real shader-mod hook. Keep the actual game
        // table intact so a retained cold thunk can delegate into this warm
        // entry exactly as it would after D3D11 initializes its own dispatch.
        auto* warmIndexed=(*reinterpret_cast<void***>(ctx.Get()))[12];
        indexedObserver.Install(warmIndexed);
        std::printf("Native indexed Detours observer installed: cold=%p warm=%p\n",
            reinterpret_cast<void*>(coldIndexed),warmIndexed);
    }
    if(inlineHooks==InlineHookMode::Early) {
        Require(RDMRenderScope::PrepareDrawHooks(dev.Get()),"early native draw bootstrap failed");
        Require(multithread->GetMultithreadProtected()==expectedProtection,
            "early native draw bootstrap changed game-context protection");
    }
    if(inlineHooks!=InlineHookMode::None)inlineObserver.Install(ctx.Get());
#else
    Require(inlineHooks==InlineHookMode::None,"inline-hook fixture requires optional Detours test dependency");
    Require(!nativeWarmDetours,"native indexed fixture requires optional Detours test dependency");
#endif
    Wrapped::Table wrapped(ctx.Get(),inlineHooks==InlineHookMode::None,!nativeLazyDispatch);
    RDMRenderScope rdm;
    bool diagnosticsEnabled=false;
    auto ArmSources=[&](ID3D11Texture2D* sceneDepth,ID3D11Texture2D* submitted) {
        float centers[4]={0.45f,0.5f,0.55f,0.5f};
        const bool armed=rdm.Arm(ctx.Get(),sceneDepth,submitted,width,height,
            {0,0,int(width/2),int(height)},{int(width/2),0,int(width-width/2),int(height)},
            {0.3f,0.6f,true,false,{}},centers,diagnosticsEnabled);
        Require(multithread->GetMultithreadProtected()==expectedProtection,"RDM Arm changed game-context protection");
        return armed;
    };
    auto Arm=[&] {
        Require(ArmSources(depth.Get(),separateSubmittedEyes?nullptr:color.Get()),"RDM arm failed");
    };
    auto Bind=[&](bool equal=true) {
        ID3D11RenderTargetView* targets[]={colorRT.Get(),motionRT.Get()};
        ctx->OMSetRenderTargets(2,targets,depthView.Get());
        ctx->OMSetDepthStencilState(equal?lit.Get():pre.Get(),0);
        ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);ctx->RSSetState(noCull.Get());
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),viewportMinDepth,viewportMaxDepth};
        ctx->RSSetViewports(1,&viewport);
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
    if(nativeLazyDispatch) {
        // Use real indexed calls for both guide production and scene shading.
        // If early hooks cover only the cold dispatch addresses, the rendered
        // image alone still looks correct: these exact counts catch that bypass.
        const UINT indices[]={0,1,2};D3D11_BUFFER_DESC indexDesc{};
        indexDesc.ByteWidth=sizeof(indices);indexDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA indexData{};indexData.pSysMem=indices;
        ComPtr<ID3D11Buffer> indexBuffer;HR(dev->CreateBuffer(&indexDesc,&indexData,&indexBuffer));
        std::array<unsigned,2> observedCalls{};
        auto IndexedFrame=[&](bool armed,bool retainedCold=false) {
            observedCalls={};
            if(nativeWarmDetours)std::printf("Native indexed Detours frame: entry=%s armed=%d\n",
                retainedCold?"retained-cold":"warm",armed);
            auto DrawIndexed=[&](unsigned pass) {
                if(nativeWarmDetours)std::printf("Native indexed call: %s %s begin\n",
                    retainedCold?"retained-cold":"warm",pass==0?"prepass":"color");
#ifdef OCU_RDM_TEST_DETOURS
                const auto before=InlineIndexed::callbacks;
                if(nativeWarmDetours) {
                    Wrapped::expected=depthView.Get();Wrapped::expectedShader=vs.Get();
                    Wrapped::check=pass==1;Wrapped::checkDepthGuide=pass==0;
                    InlineIndexed::expectedShader=pass==0?nullptr:ps.Get();
                    InlineIndexed::depthMismatches=InlineIndexed::shaderMismatches=0;InlineIndexed::observing=true;
                }
#endif
                if(retainedCold)coldIndexed(ctx.Get(),3,0,0);else ctx->DrawIndexed(3,0,0);
                if(nativeWarmDetours)std::printf("Native indexed call: %s %s returned\n",
                    retainedCold?"retained-cold":"warm",pass==0?"prepass":"color");
#ifdef OCU_RDM_TEST_DETOURS
                if(nativeWarmDetours) {
                    InlineIndexed::observing=false;Wrapped::check=false;Wrapped::checkDepthGuide=false;
                    observedCalls[pass]=InlineIndexed::callbacks-before;
                    std::printf("Native indexed observer result: callbacks=%u depthMismatches=%u shaderMismatches=%u\n",
                        observedCalls[pass],InlineIndexed::depthMismatches,InlineIndexed::shaderMismatches);
                    Require(InlineIndexed::depthMismatches==0,"warm indexed post-draw observer saw private depth");
                    Require(InlineIndexed::shaderMismatches==0,"warm indexed post-draw observer saw private pixel shader");
                    if(!retainedCold)Require(observedCalls[pass]==1,"warm indexed observer was lost or duplicated");
                    else Require(observedCalls[pass]<=1,"cold indexed delegation duplicated the warm observer");
                }
#endif
            };
            rdm.EndFrame();if(armed)Arm();
            ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH|(d24?D3D11_CLEAR_STENCIL:0),1,0xA7);
            Bind(false);ctx->IASetIndexBuffer(indexBuffer.Get(),DXGI_FORMAT_R32_UINT,0);
            ctx->PSSetShader(nullptr,nullptr,0);DrawIndexed(0);
            if(armed)Require(rdm.Stats().draws==1 && rdm.Stats().guideDraws==1 && rdm.Stats().maskedDraws==0,
                "warm native indexed prepass bypassed or duplicated the guide hook");
            const FLOAT zero[4]{};ctx->ClearRenderTargetView(colorRT.Get(),zero);ctx->ClearRenderTargetView(motionRT.Get(),zero);
            Bind();DrawIndexed(1);
            if(armed)Require(rdm.Stats().draws==2 && rdm.Stats().guideDraws==1 &&
                rdm.Stats().maskedDraws==1 && rdm.Stats().batches==1,
                "warm native indexed color draw bypassed or duplicated RDM");
            rdm.EndFrame();
            if(armed)Require(rdm.Stats().resolves==2,"warm native indexed draw missed an MRT resolve");
            Require(multithread->GetMultithreadProtected()==TRUE,"indexed native rendering changed game protection");
            return std::array<std::vector<unsigned char>,3>{Read(dev.Get(),ctx.Get(),color.Get()),
                Read(dev.Get(),ctx.Get(),motion.Get()),Read(dev.Get(),ctx.Get(),depth.Get())};
        };
        const auto nativeReference=IndexedFrame(false);
        const auto reconstructed=IndexedFrame(true);
        Require(reconstructed==nativeReference,"warm indexed reconstruction changed native color/MRT/depth bytes");
        std::puts("Native lazy-dispatch indexed fixture PASS: exact guide/draw/batch counts, native color/MRT/depth intact");
        if(nativeWarmDetours) {
            const auto coldReference=IndexedFrame(false,true);
            const auto nativeColdCallbacks=observedCalls;
            Require(coldReference==nativeReference,"retained cold indexed draw changed native color/MRT/depth bytes");
            const auto coldReconstructed=IndexedFrame(true,true);
            Require(coldReconstructed==nativeReference,"retained cold indexed RDM changed native color/MRT/depth bytes");
            Require(observedCalls==nativeColdCallbacks,"RDM changed cold-to-warm post-draw callback delivery");
            std::printf("Native indexed Detours fixture PASS: cold callbacks prepass=%u color=%u, native MRT/depth intact\n",
                observedCalls[0],observedCalls[1]);
            if(observedCalls[0]!=1 || observedCalls[1]!=1)
                std::puts("This runtime did not delegate both retained cold draws through the warm observer; cross-entry callback ordering remains unexercised here.");
        }
    }
    if(inlineHooks!=InlineHookMode::None) {
        // CSX uses a real Detours inline hook. If OCU installs outside that
        // hook, CSX's post-draw callback runs before OCU restores the game DSV.
        // Run these modes in separate processes before any other fixture has
        // populated the process-global native hook cache.
        Prepare();ctx->Draw(3,0);
        const auto nativeColor=Read(dev.Get(),ctx.Get(),color.Get());
        const auto nativeMotion=Read(dev.Get(),ctx.Get(),motion.Get());
        const auto nativeDepth=Read(dev.Get(),ctx.Get(),depth.Get());
        Prepare(true);Wrapped::expected=depthView.Get();Wrapped::expectedShader=vs.Get();Wrapped::check=true;
        const auto acceptedBefore=Wrapped::accepted;
        ctx->Draw(3,0);Wrapped::check=false;
        Require(Wrapped::accepted==acceptedBefore+1,"inline observer callback was lost or duplicated");
        Require(rdm.Stats().draws==2 && rdm.Stats().maskedDraws==1 && rdm.Stats().guideDraws==1,
            "inline hook order lost or duplicated RDM draw/guide processing");
        rdm.EndFrame();
        Require(Read(dev.Get(),ctx.Get(),color.Get())==nativeColor &&
            Read(dev.Get(),ctx.Get(),motion.Get())==nativeMotion &&
            Read(dev.Get(),ctx.Get(),depth.Get())==nativeDepth,"inline hook handoff altered native resources");
        std::printf("RDM real Detours %s-order fixture PASS: post-draw observer sees original DSV, native pixels/depth intact\n",
            inlineHooks==InlineHookMode::Early?"early-bootstrap":"late-install");
    }
    if(threading==ThreadingMode::EnableAfterInstall) {
        // Start with functioning unprotected hooks, then let the game change
        // mode between frames. Detach/rebuild only this test's vtable wrapper
        // so the transition reaches D3D11's real context table in both modes.
        auto TransitionDraw=[&] {
            Prepare(true);
            Require(rdm.Stats().draws==1 && rdm.Stats().guideDraws==1,
                "protection transition lost or duplicated the depth prepass hook");
            Wrapped::expected=depthView.Get();Wrapped::expectedShader=vs.Get();Wrapped::check=true;
            const auto acceptedBefore=Wrapped::accepted;
            ctx->Draw(3,0);Wrapped::check=false;
            Require(Wrapped::accepted==acceptedBefore+1,"protection transition lost or duplicated the post-draw observer");
            Require(rdm.Stats().draws==2 && rdm.Stats().maskedDraws==1 && rdm.Stats().batches==1,
                "protection transition lost or duplicated the scene draw hook");
            rdm.EndFrame();
            Require(rdm.Stats().resolves==2,"protection transition skipped an MRT resolve");
            return std::array<std::vector<unsigned char>,3>{Read(dev.Get(),ctx.Get(),color.Get()),
                Read(dev.Get(),ctx.Get(),motion.Get()),Read(dev.Get(),ctx.Get(),depth.Get())};
        };
        const auto beforeTransition=TransitionDraw();
        wrapped.RestoreNativeTable();
        multithread->SetMultithreadProtected(TRUE);expectedProtection=TRUE;
        Require(multithread->GetMultithreadProtected(),"game-context protection transition failed");
        wrapped.WrapCurrentTable();
        const auto afterTransition=TransitionDraw();
        Require(afterTransition==beforeTransition,"protection transition changed native color/MRT/depth results");
        std::puts("RDM multithread transition fixture PASS: exact draw/guide counts, observer, pixels and depth preserved");
    }
    Prepare();ctx->Draw(3,0);
    expectedColor=Read(dev.Get(),ctx.Get(),color.Get());expectedMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    // Confirm that both the reference and subsequent armed fixtures really
    // exercise this viewport transform rather than merely varying test labels.
    const auto mappedDepthBytes=Read(dev.Get(),ctx.Get(),depth.Get());
    float mappedDepth=0;
    if(d24) {
        unsigned packedDepth=0;std::memcpy(&packedDepth,mappedDepthBytes.data(),sizeof(packedDepth));
        mappedDepth=float(packedDepth&0x00ffffffu)/float(0x00ffffffu);
    } else std::memcpy(&mappedDepth,mappedDepthBytes.data(),sizeof(mappedDepth));
    const float expectedMappedDepth=viewportMinDepth+params[0]*(viewportMaxDepth-viewportMinDepth);
    Require(mappedDepth>expectedMappedDepth-0.0000002f && mappedDepth<expectedMappedDepth+0.0000002f,
        "prepass did not use the requested normalized viewport depth range");
    // Exercise rejection telemetry through actual native draws. Each state is
    // repeated without rebinding: one eligibility query must account for three
    // protected draws, and diagnostics must not change the GPU result.
    diagnosticsEnabled=true;
    using QueryReject=RDMRenderScope::QueryReject;
    auto RejectedCase=[&](QueryReject reason,auto configure,bool withGuide=true) {
        Prepare();configure();
        for(unsigned i=0;i<3;++i)ctx->Draw(3,0);
        const auto referenceColor=Read(dev.Get(),ctx.Get(),color.Get());
        const auto referenceMotion=Read(dev.Get(),ctx.Get(),motion.Get());
        const auto referenceDepth=Read(dev.Get(),ctx.Get(),depth.Get());
        Prepare(withGuide);if(!withGuide)Arm();configure();
        const auto before=rdm.Stats();
        for(unsigned i=0;i<3;++i)ctx->Draw(3,0);
        const auto after=rdm.Stats();
        Require(after.draws==before.draws+3,"rejected native draws were not counted");
        Require(after.protectedDraws==before.protectedDraws+3,"repeated rejected draws lost protection accounting");
        Require(after.stateQueries==before.stateQueries+1,"unchanged rejected state was queried per draw");
        Require(after.maskedDraws==before.maskedDraws && after.batches==before.batches,
            "rejection diagnostics changed draw eligibility");
        Require(after.queryAccepted==before.queryAccepted && after.startAttempts==before.startAttempts,
            "rejected draw was counted as an accepted query or batch attempt");
        for(unsigned i=0;i<RDMRenderScope::QueryRejectCount;++i) {
            Require(after.queryRejectCounts[i]==before.queryRejectCounts[i]+(i==static_cast<unsigned>(reason)?1u:0u),
                "query rejection was assigned to the wrong reason or counted per draw");
            Require(after.rejectedDrawCounts[i]==before.rejectedDrawCounts[i]+(i==static_cast<unsigned>(reason)?3u:0u),
                "cached rejected draws were assigned to the wrong reason or counted per query");
        }
        Require(after.startFailureCounts==before.startFailureCounts,"query rejection was reported as a batch failure");
        Require(after.startRejectedDraws==before.startRejectedDraws,"query rejection was counted as a rejected batch start");
        unsigned accountedProtected=after.startRejectedDraws;
        for(auto count:after.rejectedDrawCounts)accountedProtected+=count;
        Require(accountedProtected==after.protectedDraws,"protected draw breakdown does not reconcile");
        const auto& diagnostic=reason==QueryReject::SceneScope?rdm.Diagnostics().scope:rdm.Diagnostics().query;
        Require(rdm.Diagnostics().enabled && diagnostic.valid && diagnostic.queryReject==reason,
            "representative rejection diagnostic did not capture the actual color pass");
        const auto& retained=rdm.Diagnostics().byReason[static_cast<unsigned>(reason)];
        Require(retained.valid && retained.queryReject==reason && retained.hasPixelShader,
            "per-reason diagnostic did not retain the rejected color pass");
        Require(diagnostic.hasPixelShader && diagnostic.boundRtvMask==3,
            "representative diagnostic retained a depth-only pass instead of the rejected color pass");
        Require(!diagnostic.shaderCacheMismatch && !diagnostic.blendCacheMismatch,
            "normal hooked bindings were falsely diagnosed as a cache mismatch");
        if(reason==QueryReject::ShaderOutputs)
            Require(diagnostic.actualColorOutputs==1,"MRT rejection diagnostic lost the actual shader outputs");
        if(reason==QueryReject::ShaderReasons)
            Require((diagnostic.actualShaderReasons&ocu_vrs_guard::Discard)!=0,
                "shader rejection diagnostic lost the discard reason");
        if(reason==QueryReject::DepthComparison)
            Require(diagnostic.depthState.DepthFunc==D3D11_COMPARISON_LESS_EQUAL,
                "depth rejection diagnostic lost the actual comparison");
        if(reason==QueryReject::Viewport)
            Require(diagnostic.viewportCount==1 && diagnostic.firstViewport.Width==float(width/4),
                "viewport rejection diagnostic lost the actual bounds");
        if(reason==QueryReject::WriteMask)
            Require(diagnostic.targets[1].effectiveBlend.RenderTargetWriteMask==0,
                "write-mask rejection diagnostic lost the disabled attachment");
        if(reason==QueryReject::Stencil)
            Require(diagnostic.depthState.StencilEnable && diagnostic.stencilReference==1,
                "stencil rejection diagnostic lost stencil coverage state");
        if(reason==QueryReject::SceneScope)
            Require(!diagnostic.expectedDepthBound && !diagnostic.scopeMatched,
                "unrelated depth was diagnosed as the owned scene");
        Require(Read(dev.Get(),ctx.Get(),color.Get())==referenceColor,"rejected color differs from full-rate reference");
        Require(Read(dev.Get(),ctx.Get(),motion.Get())==referenceMotion,"rejected MRT differs from full-rate reference");
        Require(Read(dev.Get(),ctx.Get(),depth.Get())==referenceDepth,"rejected draw changed original depth/stencil");
        std::printf("RDM rejection fixture %s: draws=%u queries=%u\n",RDMRenderScope::QueryRejectName(reason),
            after.protectedDraws-before.protectedDraws,after.stateQueries-before.stateQueries);
        rdm.EndFrame();
    };
    RejectedCase(QueryReject::NoDepthGuide,[&]{Bind();},false);
    RejectedCase(QueryReject::ShaderReasons,[&]{Bind();ctx->PSSetShader(alpha.Get(),nullptr,0);});
    // Disabled attachments need no shader output and no reconstruction. An
    // all-zero draw still has no eligible color work and remains protected.
    D3D11_BLEND_DESC outputBlendDesc{};outputBlendDesc.IndependentBlendEnable=TRUE;
    for(auto& target:outputBlendDesc.RenderTarget) {
        target.SrcBlend=target.SrcBlendAlpha=D3D11_BLEND_ONE;
        target.DestBlend=target.DestBlendAlpha=D3D11_BLEND_ZERO;
        target.BlendOp=target.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    }
    outputBlendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> outputBlend;HR(dev->CreateBlendState(&outputBlendDesc,&outputBlend));
    auto noWriteDesc=outputBlendDesc;noWriteDesc.RenderTarget[0].RenderTargetWriteMask=0;
    ComPtr<ID3D11BlendState> noWrites;HR(dev->CreateBlendState(&noWriteDesc,&noWrites));
    RejectedCase(QueryReject::WriteMask,[&]{
        Bind();ctx->OMSetBlendState(noWrites.Get(),nullptr,0xffffffffu);
    });
    // An enabled target without a PS output remains unsafe. Probe eligibility
    // without executing the undefined native write to that missing output.
    Prepare(true);Bind();ctx->PSSetShader(one.Get(),nullptr,0);
    const auto missingColor=Read(dev.Get(),ctx.Get(),color.Get());
    const auto missingMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    const auto missingDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    const auto missingBefore=rdm.Stats();
    Require(!rdm.BeforeDraw(),"enabled attachment with no PS output was accepted");
    Require(rdm.Stats().queryRejectCounts[static_cast<unsigned>(QueryReject::ShaderOutputs)]==
        missingBefore.queryRejectCounts[static_cast<unsigned>(QueryReject::ShaderOutputs)]+1 &&
        rdm.Stats().maskedDraws==missingBefore.maskedDraws && rdm.Stats().batches==missingBefore.batches,
        "missing enabled MRT output did not retain its shader-output protection");
    const auto& missingDiagnostic=rdm.Diagnostics().byReason[static_cast<unsigned>(QueryReject::ShaderOutputs)];
    Require(missingDiagnostic.valid && missingDiagnostic.actualColorOutputs==1 &&
        missingDiagnostic.targets[1].effectiveBlend.RenderTargetWriteMask==D3D11_COLOR_WRITE_ENABLE_ALL,
        "missing-output diagnostic lost the active target mask or actual shader outputs");
    // A later, unrelated rejection must not erase the first reason's evidence.
    ctx->OMSetBlendState(noWrites.Get(),nullptr,0xffffffffu);
    Require(!rdm.BeforeDraw(),"all-zero target masks were accepted after output rejection");
    Require(rdm.Diagnostics().byReason[static_cast<unsigned>(QueryReject::ShaderOutputs)].valid &&
        rdm.Diagnostics().byReason[static_cast<unsigned>(QueryReject::ShaderOutputs)].actualColorOutputs==1 &&
        rdm.Diagnostics().byReason[static_cast<unsigned>(QueryReject::WriteMask)].valid,
        "later rejection erased earlier per-reason state");
    Require(Read(dev.Get(),ctx.Get(),color.Get())==missingColor &&
        Read(dev.Get(),ctx.Get(),motion.Get())==missingMotion &&
        Read(dev.Get(),ctx.Get(),depth.Get())==missingDepth,
        "missing-output eligibility probe changed native resources");
    rdm.EndFrame();
    RejectedCase(QueryReject::Viewport,[&]{
        Bind();D3D11_VIEWPORT partial{0,0,float(width/4),float(height),viewportMinDepth,viewportMaxDepth};
        ctx->RSSetViewports(1,&partial);
    });
    D3D11_DEPTH_STENCIL_DESC nonEqualDesc{};lit->GetDesc(&nonEqualDesc);
    nonEqualDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> nonEqual;HR(dev->CreateDepthStencilState(&nonEqualDesc,&nonEqual));
    RejectedCase(QueryReject::DepthComparison,[&]{Bind();ctx->OMSetDepthStencilState(nonEqual.Get(),0);});
    D3D11_TEXTURE2D_DESC unrelatedDesc{};depth->GetDesc(&unrelatedDesc);
    ComPtr<ID3D11Texture2D> unrelatedDepth;ComPtr<ID3D11DepthStencilView> unrelatedDSV;
    HR(dev->CreateTexture2D(&unrelatedDesc,nullptr,&unrelatedDepth));
    HR(dev->CreateDepthStencilView(unrelatedDepth.Get(),&dv,&unrelatedDSV));
    RejectedCase(QueryReject::SceneScope,[&]{
        Bind();ctx->CopyResource(unrelatedDepth.Get(),depth.Get());
        ID3D11RenderTargetView* targets[]={colorRT.Get(),motionRT.Get()};
        ctx->OMSetRenderTargets(2,targets,unrelatedDSV.Get());
    });
    if(d24) {
        D3D11_DEPTH_STENCIL_DESC stencilDesc{};lit->GetDesc(&stencilDesc);
        stencilDesc.StencilEnable=TRUE;stencilDesc.StencilReadMask=0xff;stencilDesc.StencilWriteMask=0;
        stencilDesc.FrontFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_NOT_EQUAL};
        stencilDesc.BackFace=stencilDesc.FrontFace;
        ComPtr<ID3D11DepthStencilState> stencil;HR(dev->CreateDepthStencilState(&stencilDesc,&stencil));
        RejectedCase(QueryReject::Stencil,[&]{Bind();ctx->OMSetDepthStencilState(stencil.Get(),1);});
    }
    // Query accepts this ordinary scene draw, but the resolver cannot sample
    // an RTV-only color texture. Record the failed Start separately and verify
    // the real draw still produces its full-rate color/MRT/depth result.
    D3D11_TEXTURE2D_DESC renderOnlyDesc{};color->GetDesc(&renderOnlyDesc);
    renderOnlyDesc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> renderOnlyColor;ComPtr<ID3D11RenderTargetView> renderOnlyRT;
    HR(dev->CreateTexture2D(&renderOnlyDesc,nullptr,&renderOnlyColor));
    HR(dev->CreateRenderTargetView(renderOnlyColor.Get(),nullptr,&renderOnlyRT));
    auto BindRenderOnly=[&] {
        Bind();const float zero[4]{};ctx->ClearRenderTargetView(renderOnlyRT.Get(),zero);
        ID3D11RenderTargetView* targets[]={renderOnlyRT.Get(),motionRT.Get()};
        ctx->OMSetRenderTargets(2,targets,depthView.Get());
    };
    Prepare();BindRenderOnly();ctx->Draw(3,0);
    const auto startReferenceColor=Read(dev.Get(),ctx.Get(),renderOnlyColor.Get());
    const auto startReferenceMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    const auto startReferenceDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Prepare(true);BindRenderOnly();const auto startBefore=rdm.Stats();ctx->Draw(3,0);
    const auto startAfter=rdm.Stats();
    Require(startAfter.queryAccepted==startBefore.queryAccepted+1 &&
        startAfter.queryRejectCounts==startBefore.queryRejectCounts &&
        startAfter.rejectedDrawCounts==startBefore.rejectedDrawCounts,
        "batch setup failure was misclassified as a query rejection");
    Require(startAfter.startAttempts==startBefore.startAttempts+1 &&
        startAfter.startRejectedDraws==startBefore.startRejectedDraws+1 &&
        startAfter.protectedDraws==startBefore.protectedDraws+1 &&
        startAfter.maskedDraws==startBefore.maskedDraws && startAfter.batches==startBefore.batches,
        "failed batch start lost its protected draw or claimed successful masking");
    for(unsigned i=0;i<RDMRenderScope::StartFailureCount;++i)
        Require(startAfter.startFailureCounts[i]==startBefore.startFailureCounts[i]+
            (i==static_cast<unsigned>(RDMRenderScope::StartFailure::ResolverTargets)?1u:0u),
            "RTV-only color failed at the wrong batch stage");
    Require(startAfter.lastStartFailure==RDMRenderScope::StartFailure::ResolverTargets &&
        !startAfter.lastStartHresultKnown,"resolver target rejection fabricated an HRESULT");
    const auto& startDiagnostic=rdm.Diagnostics().start;
    Require(startDiagnostic.valid && startDiagnostic.startFailure==RDMRenderScope::StartFailure::ResolverTargets &&
        startDiagnostic.queryReject==QueryReject::Count && startDiagnostic.scopeMatched && startDiagnostic.expectedDepthBound,
        "batch failure diagnostic lost the admitted scene and failed stage");
    Require(startDiagnostic.targets[0].bound && startDiagnostic.targets[0].texture &&
        startDiagnostic.targets[0].description.BindFlags==D3D11_BIND_RENDER_TARGET,
        "batch failure diagnostic lost the color texture's missing SRV capability");
    unsigned startProtected=startAfter.startRejectedDraws;
    for(auto count:startAfter.rejectedDrawCounts)startProtected+=count;
    Require(startProtected==startAfter.protectedDraws,"batch failure broke protected-draw reconciliation");
    Require(Read(dev.Get(),ctx.Get(),renderOnlyColor.Get())==startReferenceColor &&
        Read(dev.Get(),ctx.Get(),motion.Get())==startReferenceMotion &&
        Read(dev.Get(),ctx.Get(),depth.Get())==startReferenceDepth,
        "failed RDM batch start altered the native fallback draw");
    rdm.EndFrame();
    // Skyrim's mixed G-buffer passes often write RG to one attachment, R to
    // another, and nothing to several still-bound targets. Seed every channel
    // with a different per-pixel pattern: copying a neighbor's untouched B/A
    // must fail these byte-for-byte native comparisons even if written colors
    // happen to reconstruct correctly.
    auto seedCode=Compile(shader,"Seed","ps_5_0"),partialCode=Compile(shader,"Partial","ps_5_0"),
        partialOneCode=Compile(shader,"PartialOne","ps_5_0"),partialSecondCode=Compile(shader,"PartialSecond","ps_5_0");
    ComPtr<ID3D11PixelShader> seed,partial,partialOne,partialSecond;
    HR(dev->CreatePixelShader(seedCode->GetBufferPointer(),seedCode->GetBufferSize(),nullptr,&seed));
    HR(dev->CreatePixelShader(partialCode->GetBufferPointer(),partialCode->GetBufferSize(),nullptr,&partial));
    HR(dev->CreatePixelShader(partialOneCode->GetBufferPointer(),partialOneCode->GetBufferSize(),nullptr,&partialOne));
    HR(dev->CreatePixelShader(partialSecondCode->GetBufferPointer(),partialSecondCode->GetBufferSize(),nullptr,&partialSecond));
    Require(ocu_exact_pixels::Mark(seed.Get()),"native seed shader exact-pixel request failed");
    Require(ocu_vrs_guard::ShaderColorOutputs(partialOne.Get())==1 &&
        ocu_vrs_guard::ShaderColorOutputs(partialSecond.Get())==2,"single-attachment output classification failed");
    auto MakeWriteMask=[&](UINT8 first,UINT8 second) {
        auto desc=outputBlendDesc;desc.RenderTarget[0].RenderTargetWriteMask=first;
        desc.RenderTarget[1].RenderTargetWriteMask=second;
        ComPtr<ID3D11BlendState> state;HR(dev->CreateBlendState(&desc,&state));return state;
    };
    ComPtr<ID3D11Texture2D> disabledTexture;ComPtr<ID3D11RenderTargetView> disabledView;
    auto disabledDesc=renderOnlyDesc;disabledDesc.Format=auxiliaryFormat;
    HR(dev->CreateTexture2D(&disabledDesc,nullptr,&disabledTexture));
    HR(dev->CreateRenderTargetView(disabledTexture.Get(),nullptr,&disabledView));
    auto PartialMrtCase=[&](const char* name,UINT8 first,UINT8 second,ID3D11PixelShader* candidate,
        bool disabledNoSrv=false,bool changeMasks=false) {
        auto state=MakeWriteMask(first,second);
        auto changedState=MakeWriteMask(D3D11_COLOR_WRITE_ENABLE_BLUE,D3D11_COLOR_WRITE_ENABLE_GREEN);
        ID3D11Texture2D* auxiliary=disabledNoSrv?disabledTexture.Get():motion.Get();
        ID3D11RenderTargetView* auxiliaryView=disabledNoSrv?disabledView.Get():motionRT.Get();
        auto SeedAndBind=[&] {
            ID3D11RenderTargetView* targets[]={colorRT.Get(),auxiliaryView};
            ctx->OMSetRenderTargets(2,targets,depthView.Get());
            ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);
            ctx->PSSetShader(seed.Get(),nullptr,0);ctx->Draw(3,0);
            ctx->PSSetShader(candidate,nullptr,0);ctx->OMSetBlendState(state.Get(),nullptr,0xffffffffu);
        };
        Prepare();SeedAndBind();ctx->Draw(3,0);
        if(changeMasks) {ctx->OMSetBlendState(changedState.Get(),nullptr,0xffffffffu);ctx->Draw(3,0);}
        const auto nativeColor=Read(dev.Get(),ctx.Get(),color.Get());
        const auto nativeAuxiliary=Read(dev.Get(),ctx.Get(),auxiliary);
        const auto nativeDepth=Read(dev.Get(),ctx.Get(),depth.Get());
        Prepare(true);SeedAndBind();
        const auto partialBefore=rdm.Stats();
        // Install the mask before measuring so shader invocation counts cover
        // only the real scene draw, excluding setup and reconstruction work.
        Require(PrepareCoveredDraw(rdm,ctx.Get()),"partial-write color pass was rejected");
        D3D11_QUERY_DESC queryDesc{D3D11_QUERY_PIPELINE_STATISTICS,0};
        ComPtr<ID3D11Query> sceneQuery;HR(dev->CreateQuery(&queryDesc,&sceneQuery));
        ctx->Begin(sceneQuery.Get());ctx->Draw(3,0);ctx->End(sceneQuery.Get());rdm.AfterDraw(true);
        const auto firstBatch=rdm.Stats();
        Require(firstBatch.maskedDraws==partialBefore.maskedDraws+1 &&
            firstBatch.batches==partialBefore.batches+1 && firstBatch.startAttempts==partialBefore.startAttempts+1,
            "partial MRT test fell back to full-rate without a successful batch");
        const unsigned activeTargets=(first!=0)+(second!=0);
        const unsigned partialDraw=(first && first!=D3D11_COLOR_WRITE_ENABLE_ALL) ||
            (second && second!=D3D11_COLOR_WRITE_ENABLE_ALL);
        const unsigned inactiveDraw=!first || !second;
        Require(firstBatch.partialMaskDraws==partialBefore.partialMaskDraws+partialDraw &&
            firstBatch.inactiveTargetDraws==partialBefore.inactiveTargetDraws+inactiveDraw,
            "new MRT admission counters did not describe the successful draw");
        if(changeMasks) {
            ctx->OMSetBlendState(changedState.Get(),nullptr,0xffffffffu);ctx->Draw(3,0);
            Require(rdm.Stats().batches==partialBefore.batches+2 &&
                rdm.Stats().maskedDraws==partialBefore.maskedDraws+2,
                "write-mask change reused the previous reconstruction batch");
            Require(rdm.Stats().resolves==partialBefore.resolves+activeTargets,
                "write-mask change did not flush the previous active MRTs before drawing");
        }
        rdm.EndFrame();
        Require(rdm.Stats().partialMaskDraws==partialBefore.partialMaskDraws+partialDraw+(changeMasks?1u:0u) &&
            rdm.Stats().inactiveTargetDraws==partialBefore.inactiveTargetDraws+inactiveDraw,
            "MRT admission counters changed during resolve or lost the second partial draw");
        Require(rdm.Stats().resolves==partialBefore.resolves+activeTargets+(changeMasks?2u:0u),
            "disabled target was resolved or active partial target was skipped");
        Require(Read(dev.Get(),ctx.Get(),color.Get())==nativeColor,
            "partial MRT resolve changed a written or untouched color channel");
        Require(Read(dev.Get(),ctx.Get(),auxiliary)==nativeAuxiliary,
            "partial MRT resolve changed a written or untouched auxiliary channel");
        Require(Read(dev.Get(),ctx.Get(),depth.Get())==nativeDepth,
            "partial MRT handoff changed original depth/stencil");
        D3D11_QUERY_DATA_PIPELINE_STATISTICS measured{};HRESULT ready=S_FALSE;
        for(unsigned i=0;i<100000 && ready==S_FALSE;++i)
            ready=ctx->GetData(sceneQuery.Get(),&measured,sizeof(measured),0);
        HR(ready);Require(ready==S_OK && measured.PSInvocations>0,"partial MRT scene query produced no result");
        if(driver!=D3D_DRIVER_TYPE_WARP)
            Require(measured.PSInvocations<width*height,"partial MRT batch did not reduce hardware pixel shading");
        std::printf("RDM partial MRT fixture %s: masks=%X/%X sceneInvocations=%llu full=%u batches=%u resolves=%u\n",
            name,unsigned(first),unsigned(second),measured.PSInvocations,width*height,
            rdm.Stats().batches-partialBefore.batches,rdm.Stats().resolves-partialBefore.resolves);
    };
    PartialMrtCase("RG/R preserves BA/GBA",D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN,
        D3D11_COLOR_WRITE_ENABLE_RED,partial.Get());
    PartialMrtCase("disabled omitted output",D3D11_COLOR_WRITE_ENABLE_ALL,0,partialOne.Get());
    PartialMrtCase("disabled RTV-only omitted output",D3D11_COLOR_WRITE_ENABLE_ALL,0,partialOne.Get(),true);
    PartialMrtCase("disabled target zero keeps slot one",0,D3D11_COLOR_WRITE_ENABLE_ALL,partialSecond.Get());
    PartialMrtCase("mask change flushes prior channels",D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN,
        D3D11_COLOR_WRITE_ENABLE_RED,partial.Get(),false,true);
    Prepare(true);
    const auto acceptedBefore=rdm.Stats();
    ctx->Draw(3,0);ctx->Draw(3,0);
    const auto acceptedAfter=rdm.Stats();
    Require(acceptedAfter.queryAccepted==acceptedBefore.queryAccepted+1 &&
        acceptedAfter.startAttempts==acceptedBefore.startAttempts+1 && acceptedAfter.batches==acceptedBefore.batches+1,
        "accepted draws did not record one successful batch attempt");
    Require(acceptedAfter.maskedDraws==acceptedBefore.maskedDraws+2 &&
        acceptedAfter.startFailureCounts==acceptedBefore.startFailureCounts &&
        acceptedAfter.rejectedDrawCounts==acceptedBefore.rejectedDrawCounts &&
        acceptedAfter.startRejectedDraws==acceptedBefore.startRejectedDraws,
        "accepted batch telemetry reported a failure or lost a masked draw");
    CheckColor();rdm.EndFrame();
    // Repeated null-PS prepass draws keep the same game state after guide
    // restoration. Neither guide admission nor color rejection needs querying again.
    Prepare(true);Bind(false);ctx->PSSetShader(nullptr,nullptr,0);
    const auto guideCacheBefore=rdm.Stats();
    ctx->Draw(3,0);ctx->Draw(3,0);ctx->Draw(3,0);
    const auto guideCacheAfter=rdm.Stats();
    Require(guideCacheAfter.guideDraws==guideCacheBefore.guideDraws+3 &&
        guideCacheAfter.guideStateQueries==guideCacheBefore.guideStateQueries+1 &&
        guideCacheAfter.stateQueries==guideCacheBefore.stateQueries+1,
        "restored depth-prepass state was redundantly queried or lost guide draws");
    ctx->Draw(0,0);ctx->DrawInstanced(3,0,0,0);
    Require(rdm.Stats().draws==guideCacheAfter.draws &&
        rdm.Stats().guideDraws==guideCacheAfter.guideDraws &&
        rdm.Stats().guideStateQueries==guideCacheAfter.guideStateQueries,
        "empty direct draws performed guide or admission work");
    Bind();ctx->Draw(3,0);CheckColor();rdm.EndFrame();

    // Populate every guide-admission cache, then change one native binding at
    // a time. Hazard checks run before a draw so deliberately unsupported
    // stream-output, predication and read-only-depth combinations are never
    // executed. Their removal must recover on the immediately following draw.
    {
        auto PrimeGuide=[&] {
            Prepare(true);Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
        };
        auto HazardCase=[&](const char* label,auto install,auto remove) {
            PrimeGuide();const auto cached=rdm.Stats();install();
            Require(!rdm.BeginDepthGuide(false),label);
            const auto rejected=rdm.Stats();
            Require(rejected.guideDraws==cached.guideDraws &&
                rejected.guideResets==cached.guideResets+1 &&
                rejected.guideStateQueries==cached.guideStateQueries+1,
                "changed hazard reused stale guide admission or failed to clear ownership");
            remove();ctx->Draw(3,0);
            Require(rdm.Stats().guideDraws==cached.guideDraws+1,
                "guide admission did not recover immediately after removing hazard");
            Bind();ctx->Draw(3,0);CheckColor();rdm.EndFrame();
        };
        D3D11_QUERY_DESC predicateDesc{D3D11_QUERY_OCCLUSION_PREDICATE,0};
        ComPtr<ID3D11Predicate> predicate;HR(dev->CreatePredicate(&predicateDesc,&predicate));
        HazardCase("cached guide ignored predication",
            [&]{ctx->SetPredication(predicate.Get(),TRUE);},[&]{ctx->SetPredication(nullptr,FALSE);});

        D3D11_BUFFER_DESC streamDesc{};streamDesc.ByteWidth=256;streamDesc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
        ComPtr<ID3D11Buffer> stream;HR(dev->CreateBuffer(&streamDesc,nullptr,&stream));
        const UINT streamOffset=0;ID3D11Buffer* streamTarget=stream.Get();
        HazardCase("cached guide ignored stream-output binding",
            [&]{ctx->SOSetTargets(1,&streamTarget,&streamOffset);},[&]{ctx->SOSetTargets(0,nullptr,nullptr);});

        D3D11_BUFFER_DESC uavDesc{};uavDesc.ByteWidth=64;uavDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
        uavDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;uavDesc.StructureByteStride=4;
        ComPtr<ID3D11Buffer> unorderedBuffer;HR(dev->CreateBuffer(&uavDesc,nullptr,&unorderedBuffer));
        D3D11_UNORDERED_ACCESS_VIEW_DESC unorderedDesc{};unorderedDesc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
        unorderedDesc.Buffer.NumElements=16;
        ComPtr<ID3D11UnorderedAccessView> unordered;HR(dev->CreateUnorderedAccessView(unorderedBuffer.Get(),&unorderedDesc,&unordered));
        ID3D11UnorderedAccessView* unorderedTarget=unordered.Get();ID3D11UnorderedAccessView* noUnordered=nullptr;
        HazardCase("cached guide ignored KEEP-render-targets OM UAV update",
            [&]{ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                nullptr,nullptr,2,1,&unorderedTarget,nullptr);},
            [&]{ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                nullptr,nullptr,2,1,&noUnordered,nullptr);});

        auto readOnlyDesc=dv;readOnlyDesc.Flags=D3D11_DSV_READ_ONLY_DEPTH;
        ComPtr<ID3D11DepthStencilView> readOnlyDepth;HR(dev->CreateDepthStencilView(depth.Get(),&readOnlyDesc,&readOnlyDepth));
        ID3D11RenderTargetView* targets[]={colorRT.Get(),motionRT.Get()};
        HazardCase("cached guide ignored read-only depth view",
            [&]{ctx->OMSetRenderTargets(2,targets,readOnlyDepth.Get());},
            [&]{ctx->OMSetRenderTargets(2,targets,depthView.Get());});
        HazardCase("cached guide ignored sample mask change",
            [&]{ctx->OMSetBlendState(nullptr,nullptr,0xfffffffeu);},
            [&]{ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);});
        D3D11_BLEND_DESC coverageDesc{};coverageDesc.AlphaToCoverageEnable=TRUE;
        coverageDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        ComPtr<ID3D11BlendState> coverageBlend;HR(dev->CreateBlendState(&coverageDesc,&coverageBlend));
        HazardCase("cached guide ignored alpha-to-coverage change",
            [&]{ctx->OMSetBlendState(coverageBlend.Get(),nullptr,0xffffffffu);},
            [&]{ctx->OMSetBlendState(nullptr,nullptr,0xffffffffu);});

        PrimeGuide();const auto beforeShader=rdm.Stats();
        ctx->PSSetShader(ps.Get(),nullptr,0);
        Require(!rdm.BeginDepthGuide(false),"PS replacement retained null-shader guide route");
        Require(rdm.BeginDepthGuide(true),"ordinary PS replacement did not select invalidation route");
        ctx->Draw(3,0);rdm.EndDepthGuide();
        Require(rdm.Stats().guideInvalidationDraws==beforeShader.guideInvalidationDraws+1 &&
            rdm.Stats().guideDepthReads==beforeShader.guideDepthReads &&
            rdm.Stats().guideTargetReads==beforeShader.guideTargetReads,
            "shader-only change redundantly read depth/targets or missed invalidation");
        ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
        Require(rdm.Stats().guideDraws==beforeShader.guideDraws+1,
            "null PS restoration did not recover same-draw ownership");
        Bind();ctx->Draw(3,0);CheckColor();rdm.EndFrame();

        PrimeGuide();const auto beforeRaster=rdm.Stats();
        ctx->RSSetState(noCull.Get());D3D11_RECT fullScissor{0,0,LONG(width),LONG(height)};
        ctx->RSSetScissorRects(1,&fullScissor);ctx->Draw(3,0);
        Require(rdm.Stats().guideDraws==beforeRaster.guideDraws+1 &&
            rdm.Stats().guideStateQueries==beforeRaster.guideStateQueries &&
            rdm.Stats().guideHazardReads==beforeRaster.guideHazardReads &&
            rdm.Stats().guideDepthReads==beforeRaster.guideDepthReads &&
            rdm.Stats().guideTargetReads==beforeRaster.guideTargetReads,
            "raster/scissor-only setters redundantly queried guide admission");
        const auto beforeViewport=rdm.Stats();
        D3D11_VIEWPORT fullViewport{0,0,float(width),float(height),viewportMinDepth,viewportMaxDepth};
        ctx->RSSetViewports(1,&fullViewport);ctx->Draw(3,0);
        Require(rdm.Stats().guideDraws==beforeViewport.guideDraws+1 &&
            rdm.Stats().guideDepthReads==beforeViewport.guideDepthReads &&
            rdm.Stats().guideTargetReads==beforeViewport.guideTargetReads,
            "shared guard viewport callback read unrelated depth/targets");
        ctx->OMSetDepthStencilState(lit.Get(),0);
        Require(!rdm.BeginDepthGuide(false),"depth-equal state reused writable-depth guide eligibility");
        ctx->OMSetDepthStencilState(pre.Get(),0);ctx->Draw(3,0);
        Require(rdm.Stats().guideDraws==beforeViewport.guideDraws+2,
            "writable depth restoration did not recover guide admission");
        Bind();ctx->Draw(3,0);CheckColor();rdm.EndFrame();
        std::puts("RDM granular guide cache PASS: predicate, SO, OM UAV, read-only DSV, sample mask, alpha coverage, PS/depth recovery; raster/scissor reuse");
    }

    // An empty guide cannot contain stale ownership. Color/depth writers must
    // render normally without a pointless geometry replay, then recover on a prepass.
    Prepare();Bind(false);ctx->PSSetShader(alpha.Get(),nullptr,0);
    ctx->Draw(3,0);ctx->Draw(3,0);ctx->Draw(3,0);
    const auto emptyGuideColor=Read(dev.Get(),ctx.Get(),color.Get());
    const auto emptyGuideMotion=Read(dev.Get(),ctx.Get(),motion.Get());
    const auto emptyGuideDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Prepare();Arm();Bind(false);ctx->PSSetShader(alpha.Get(),nullptr,0);
    ctx->Draw(3,0);ctx->Draw(3,0);ctx->Draw(3,0);
    Require(rdm.Stats().guideDraws==0 && rdm.Stats().guideInvalidationDraws==0 &&
        rdm.Stats().guideStateQueries==1 && rdm.Stats().emptyGuideSkips==3,
        "empty guide caused invalidation replay or repeated state inspection");
    Require(Read(dev.Get(),ctx.Get(),color.Get())==emptyGuideColor &&
        Read(dev.Get(),ctx.Get(),motion.Get())==emptyGuideMotion &&
        Read(dev.Get(),ctx.Get(),depth.Get())==emptyGuideDepth,
        "empty-guide fast path changed native color, motion or depth");
    Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
    Bind();ctx->Draw(3,0);Require(rdm.Stats().maskedDraws==1,"empty guide cache did not recover after a real prepass");
    CheckColor();rdm.EndFrame();

    // Consumer boundaries need fresh color reconstruction, but equality-only
    // scene draws preserve the prepared depth mask across those separate batches.
    Prepare(true);const auto reuseDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Bind();ctx->Draw(3,0);CheckColor();
    ctx->Draw(3,0);CheckColor();
    Require(rdm.Stats().batches==2 && rdm.Stats().maskPreparations==1 && rdm.Stats().maskReuses==1,
        "unchanged depth/guide rebuilt the mask at a color consumer boundary");
    Require(Read(dev.Get(),ctx.Get(),depth.Get())==reuseDepth,"mask reuse modified scene depth");
    // Even an equal-valued new prepass changes ownership and requires preparation.
    Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
    Bind();ctx->Draw(3,0);CheckColor();
    Require(rdm.Stats().maskPreparations==2 && rdm.Stats().maskReuses==1,
        "new depth ownership reused stale prepared mask coverage");
    ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,.4f,0);
    Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
    Bind();ctx->Draw(3,0);CheckColor();
    Require(rdm.Stats().maskPreparations==3 && Read(dev.Get(),ctx.Get(),depth.Get())==reuseDepth,
        "depth-clear recovery reused stale coverage or modified the native depth/stencil");
    rdm.EndFrame();
    std::puts("RDM work reuse fixtures PASS: guide state cache, empty-guide skips, mask reuse and depth recovery");
    {
        RDMRenderScope timedScope;
        const float centers[4]{.45f,.5f,.55f,.5f};
        auto TimedArm=[&](bool sample) {
            Require(timedScope.Arm(ctx.Get(),depth.Get(),separateSubmittedEyes?nullptr:color.Get(),width,height,
                {0,0,int(width/2),int(height)},{int(width/2),0,int(width-width/2),int(height)},
                {.3f,.6f,true,false,{}},centers,sample),"timed RDM arm failed");
        };
        Prepare();TimedArm(true);
        Bind(false);ctx->PSSetShader(nullptr,nullptr,0);ctx->Draw(3,0);
        Bind();ctx->Draw(3,0);CheckColor();
        ctx->Draw(3,0);CheckColor();
        std::printf("Timed coverage: draws=%u captures=%u coverageReuses=%u preparations=%u maskReuses=%u\n",
            timedScope.Stats().draws,timedScope.Stats().colorCoverageDraws,timedScope.Stats().colorCoverageReuses,
            timedScope.Stats().maskPreparations,timedScope.Stats().maskReuses);
        Require(timedScope.Stats().maskPreparations==1 && timedScope.Stats().maskReuses==1,
            "timed RDM changed mask reuse");
        timedScope.EndFrame();
        RDMRenderScope::GpuSample gpu;
        Require(!timedScope.TakeGpuSample(gpu),"timed RDM exposed same-frame query results");
        // Only this fixture drains/waits; production polling never does either.
        ctx->Flush();
        const auto deadline=GetTickCount64()+5000;
        while(!timedScope.TakeGpuSample(gpu)) {
            Require(GetTickCount64()<deadline,"timed RDM failed to deliver its older sample");
            TimedArm(false);timedScope.EndFrame();Sleep(1);
        }
        Require(gpu.sampleId==1 && gpu.preparationSegments==1 && gpu.resolveSegments==2,
            "timed RDM stage counts do not match prepared/reused batches");
        Require(gpu.preparationMs>=0 && gpu.resolveMs>=0 &&
            gpu.frameSpanMs+1.e-9>=gpu.preparationMs+gpu.resolveMs,"invalid integrated GPU sample");
        Require(!timedScope.TakeGpuSample(gpu),"timed RDM delivered one sample twice");
        CheckColor();
        std::puts("RDM sampled GPU timing integration PASS: unchanged pixels, one preparation, two resolves, delayed delivery once");
    }
    // Diagnostics are opt-in. A fresh Arm resets prior counters and snapshots,
    // and the same accepted GPU path still works with descriptor capture off.
    diagnosticsEnabled=false;Arm();
    const auto reset=rdm.Stats();
    Require(reset.draws==0 && reset.stateQueries==0 && reset.queryAccepted==0 && reset.startAttempts==0 &&
        reset.diagnosticSamples==0 && reset.shaderCacheMismatchSamples==0 && reset.blendCacheMismatchSamples==0 &&
        reset.guideStateQueries==0 && reset.emptyGuideSkips==0 && reset.maskPreparations==0 && reset.maskReuses==0,
        "Arm retained telemetry from the previous frame");
    for(auto count:reset.queryRejectCounts)Require(count==0,"Arm retained a query rejection counter");
    for(auto count:reset.rejectedDrawCounts)Require(count==0,"Arm retained a rejected draw counter");
    for(auto count:reset.startFailureCounts)Require(count==0,"Arm retained a batch failure counter");
    Require(reset.startRejectedDraws==0,"Arm retained a rejected batch-start draw counter");
    Require(!rdm.Diagnostics().enabled && !rdm.Diagnostics().scope.valid && !rdm.Diagnostics().query.valid &&
        !rdm.Diagnostics().start.valid,"disabled diagnostics retained an old representative sample");
    Prepare(true);ctx->Draw(3,0);CheckColor();
    Require(rdm.Stats().maskedDraws==1 && rdm.Stats().diagnosticSamples==0,
        "disabled diagnostics changed masking or sampled descriptors");
    rdm.EndFrame();
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
        Bind();D3D11_VIEWPORT offscreen{0,0,float(width/4),float(height/2),viewportMinDepth,viewportMaxDepth};
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
    // Consecutive compatible draws retain private depth until a mutation or
    // consumer boundary. Read-only getters expose the real game DSV without
    // changing the physical binding or resolving pending color. Mutation
    // boundaries must physically restore native state before their call.
    Prepare(true);
    const auto retainedDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    for(unsigned draw=0;draw<4;++draw)ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==4 && rdm.Stats().batches==1 &&
        rdm.Stats().privateDepthBinds==1 && rdm.Stats().originalDepthRestores==0,
        "compatible scene draws did not retain one private depth binding");
    Require(rdm.Stats().resolves==0,"compatible draws resolved their MRTs prematurely");
    auto VerifyNativeTargets=[&](ID3D11DepthStencilView* expected,bool withUavs=false) {
        ID3D11RenderTargetView* raw[2]{};ComPtr<ID3D11DepthStencilView> gotDepth;
        if(withUavs)ctx->OMGetRenderTargetsAndUnorderedAccessViews(2,raw,&gotDepth,0,0,nullptr);
        else ctx->OMGetRenderTargets(2,raw,&gotDepth);
        ComPtr<ID3D11RenderTargetView> first,second;first.Attach(raw[0]);second.Attach(raw[1]);
        Require(gotDepth.Get()==expected && first.Get()==colorRT.Get() && second.Get()==motionRT.Get(),
            "output-merger getter exposed private depth or changed native MRTs");
    };
    VerifyNativeTargets(depthView.Get());
    Require(rdm.Stats().originalDepthRestores==0 && rdm.Stats().resolves==0,
        "read-only OM depth getter restored private depth or resolved pending color");
    ctx->Draw(3,0);
    Require(rdm.Stats().privateDepthBinds==1 && rdm.Stats().originalDepthRestores==0,
        "read-only OM getter broke private-depth reuse for the next draw");
    VerifyNativeTargets(depthView.Get(),true);
    Require(rdm.Stats().originalDepthRestores==0 && rdm.Stats().resolves==0,
        "read-only OM/UAV getter restored private depth or resolved pending color");
    ctx->Draw(3,0);
    Require(rdm.Stats().privateDepthBinds==1,"read-only OM/UAV getter broke private-depth reuse");
    // Shader mods update tracked state between draws, even when the effective
    // reconstruction contract is unchanged. Revalidation must preserve reuse.
    D3D11_DEPTH_STENCIL_DESC equivalentDepthDesc{};lit->GetDesc(&equivalentDepthDesc);
    ComPtr<ID3D11DepthStencilState> equivalentDepth;
    HR(dev->CreateDepthStencilState(&equivalentDepthDesc,&equivalentDepth));
    D3D11_BLEND_DESC equivalentBlendDesc{};equivalentBlendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    equivalentBlendDesc.RenderTarget[0].SrcBlend=equivalentBlendDesc.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
    equivalentBlendDesc.RenderTarget[0].DestBlend=equivalentBlendDesc.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_ZERO;
    equivalentBlendDesc.RenderTarget[0].BlendOp=equivalentBlendDesc.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
    ComPtr<ID3D11BlendState> equivalentBlend;HR(dev->CreateBlendState(&equivalentBlendDesc,&equivalentBlend));
    ComPtr<ID3D11RasterizerState> equivalentRasterizer;HR(dev->CreateRasterizerState(&noCullDesc,&equivalentRasterizer));
    ctx->PSSetShader(strippedPS.Get(),nullptr,0);ctx->Draw(3,0);
    ctx->OMSetDepthStencilState(equivalentDepth.Get(),0);ctx->Draw(3,0);
    ctx->OMSetBlendState(equivalentBlend.Get(),nullptr,0xffffffffu);ctx->Draw(3,0);
    D3D11_VIEWPORT equivalentViewport{0,0,float(width),float(height),viewportMinDepth,viewportMaxDepth};
    ctx->RSSetViewports(1,&equivalentViewport);ctx->RSSetState(equivalentRasterizer.Get());ctx->Draw(3,0);
    Require(rdm.Stats().maskedDraws==10 && rdm.Stats().batches==1 && rdm.Stats().privateDepthBinds==1 &&
        rdm.Stats().originalDepthRestores==0 && rdm.Stats().resolves==0,
        "compatible shader/depth/blend/viewport/rasterizer revalidation broke retained-depth reuse");
    rdm.EndFrame();
    Require(rdm.Stats().originalDepthRestores==1 && rdm.Stats().resolves==2,
        "EndFrame left retained private depth bound or failed to finish pending MRTs");
    VerifyNativeTargets(depthView.Get());
    Require(rdm.Stats().originalDepthRestores==1,"verification getter changed the completed frame binding");
    CheckColor();Require(Read(dev.Get(),ctx.Get(),depth.Get())==retainedDepth,
        "retained private depth altered native scene depth/stencil");

    // Each boundary starts with one pending draw. The original DSV must already
    // be physically restored by the boundary; returning it from a getter alone
    // cannot satisfy the restoration counter assertions.
    D3D11_TEXTURE2D_DESC retainedUavDesc{};retainedUavDesc.Width=retainedUavDesc.Height=4;
    retainedUavDesc.MipLevels=retainedUavDesc.ArraySize=retainedUavDesc.SampleDesc.Count=1;
    retainedUavDesc.Format=DXGI_FORMAT_R32_UINT;retainedUavDesc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> retainedUavTexture;ComPtr<ID3D11UnorderedAccessView> retainedUav;
    HR(dev->CreateTexture2D(&retainedUavDesc,nullptr,&retainedUavTexture));
    HR(dev->CreateUnorderedAccessView(retainedUavTexture.Get(),nullptr,&retainedUav));
    for(unsigned boundary=0;boundary<10;++boundary) {
        Prepare(true);ctx->Draw(3,0);
        Require(rdm.Stats().privateDepthBinds==1 && rdm.Stats().originalDepthRestores==0,
            "boundary fixture did not begin with retained private depth");
        ID3D11DepthStencilView* expected=depthView.Get();
        ID3D11RenderTargetView* targets[]{colorRT.Get(),motionRT.Get()};
        if(boundary==0) { auto copied=Read(dev.Get(),ctx.Get(),color.Get());Require(copied==expectedColor,"copy consumed unresolved sparse color"); }
        if(boundary==1)ctx->ClearDepthStencilView(depthView.Get(),D3D11_CLEAR_DEPTH,.4f,0);
        if(boundary==2)ctx->Dispatch(0,0,0);
        if(boundary==3)rdm.EndFrame();
        if(boundary==4) { expected=unrelatedDSV.Get();ctx->OMSetRenderTargets(2,targets,expected); }
        if(boundary==5) { expected=unrelatedDSV.Get();ctx->OMSetRenderTargetsAndUnorderedAccessViews(2,targets,expected,0,0,nullptr,nullptr); }
        if(boundary==6) { ctx->PSSetShader(alpha.Get(),nullptr,0);ctx->Draw(3,0);Require(rdm.Stats().maskedDraws==1,"protected alpha draw used retained private depth"); }
        if(boundary==7)ctx->PSSetShader(strippedPS.Get(),nullptr,0);
        if(boundary==8) {
            auto* view=retainedUav.Get();
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                nullptr,nullptr,2,1,&view,nullptr);
        }
        if(boundary==9) {
            ComPtr<ID3D11DepthStencilView> queried;
            ctx->OMGetRenderTargets(0,nullptr,&queried);
            Require(queried.Get()==depthView.Get() && rdm.Stats().originalDepthRestores==0,
                "saved OM state did not virtualize the original DSV read-only");
            ctx->OMSetRenderTargets(2,targets,queried.Get());
        }
        if(boundary==7) {
            Require(rdm.Stats().originalDepthRestores==0,"compatible PS setter restored depth before revalidation");
            VerifyNativeTargets(expected);
            ctx->Draw(3,0);
            Require(rdm.Stats().maskedDraws==2 && rdm.Stats().privateDepthBinds==1 && rdm.Stats().originalDepthRestores==0,
                "compatible PS revalidation failed to retain private depth");
            rdm.EndFrame();CheckColor();
            Require(Read(dev.Get(),ctx.Get(),depth.Get())==retainedDepth,"compatible PS revalidation changed scene depth");
            continue;
        }
        Require(rdm.Stats().originalDepthRestores==1,"consumer/state boundary left private depth physically bound");
        VerifyNativeTargets(expected);
        Require(rdm.Stats().originalDepthRestores==1,"getter changed the physical boundary restoration count");
        if(boundary==8) {
            ComPtr<ID3D11UnorderedAccessView> gotUav;
            ctx->OMGetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,2,1,&gotUav);
            Require(gotUav.Get()==retainedUav.Get(),"restoring private depth lost a KEEP-targets UAV update");
        }
        if(boundary==9) {
            ctx->Draw(3,0);
            Require(rdm.Stats().maskedDraws==2 && rdm.Stats().privateDepthBinds==2,
                "shader/saved-state restoration failed to resume retained-depth rendering");
        }
        rdm.EndFrame();CheckColor();
        if(boundary==8) {
            ID3D11UnorderedAccessView* empty=nullptr;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL,
                nullptr,nullptr,2,1,&empty,nullptr);
        }
        if(boundary!=1)Require(Read(dev.Get(),ctx.Get(),depth.Get())==retainedDepth,
            "retained-depth consumer boundary changed original depth/stencil");
    }
    // Context-state capture must not export the private DSV into a state object
    // which the application may restore after RDM is disarmed.
    ComPtr<ID3D11Device1> retainedDevice1;ComPtr<ID3D11DeviceContext1> retainedContext1;
    HR(dev.As(&retainedDevice1));HR(ctx.As(&retainedContext1));
    ComPtr<ID3DDeviceContextState> freshState,capturedState;
    HR(retainedDevice1->CreateDeviceContextState(0,&level,1,D3D11_SDK_VERSION,
        __uuidof(ID3D11Device),nullptr,&freshState));
    Prepare(true);ctx->Draw(3,0);
    retainedContext1->SwapDeviceContextState(freshState.Get(),&capturedState);
    Require(rdm.Stats().originalDepthRestores==1 && rdm.Stats().resolves==2,
        "context-state capture did not restore original depth and finish color first");
    rdm.EndFrame();
    retainedContext1->SwapDeviceContextState(capturedState.Get(),nullptr);
    VerifyNativeTargets(depthView.Get());
    Require(rdm.Stats().originalDepthRestores==1,
        "restoring captured context state leaked a retained private depth binding");
    CheckColor();Require(Read(dev.Get(),ctx.Get(),depth.Get())==retainedDepth,
        "context-state capture changed native scene depth/stencil");
    std::puts("RDM retained depth PASS: compatible/getter reuse, state rebind, consumer/EndFrame restoration, context-state capture");
    Prepare(true);auto expectedDepth=Read(dev.Get(),ctx.Get(),depth.Get());
    Bind();
    // Measure the scene draw alone, excluding mask setup and reconstruction.
    Require(PrepareCoveredDraw(rdm,ctx.Get()),"eligible color pass rejected");
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
    ComPtr<ID3D11DepthStencilView> clearedDepth;ctx->OMGetRenderTargets(0,nullptr,&clearedDepth);
    Require(!clearedDepth,"retained-depth restoration undid application ClearState");
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
    // A full depth prepass is not evidence that a later color draw covers the
    // same pixels. Preserve a varying background outside each color rectangle,
    // including partial clusters and geometry thinner than a sampling cluster.
    for (const auto bounds : {std::array<UINT,2>{0,width/4},
             std::array<UINT,2>{0,width/4-3},
             std::array<UINT,2>{width/2-3,width/2+3},
             std::array<UINT,2>{width/4,width/4+1}}) {
        auto PartialColor=[&](bool enabled) {
            Prepare(enabled);
            std::vector<unsigned char> background(width*height*4);
            for(UINT y=0;y<height;++y) for(UINT x=0;x<width;++x) {
                const UINT p=(y*width+x)*4;
                background[p]=static_cast<unsigned char>(x*13+y*7);
                background[p+1]=static_cast<unsigned char>(x*3+y*19);
                background[p+2]=static_cast<unsigned char>(x*23+y*11);
                background[p+3]=255;
            }
            ctx->UpdateSubresource(color.Get(),0,nullptr,background.data(),width*4,0);
            ctx->VSSetShader(meshVS.Get(),nullptr,0);
            ctx->PSSetShader(meshPS.Get(),nullptr,0);
            params[2]=float(bounds[0]);params[3]=float(bounds[1]);
            ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);
            ctx->Draw(6,0);rdm.EndFrame();
            return std::array{Read(dev.Get(),ctx.Get(),color.Get()),
                Read(dev.Get(),ctx.Get(),motion.Get()),Read(dev.Get(),ctx.Get(),depth.Get())};
        };
        const auto native=PartialColor(false);
        const auto reconstructed=PartialColor(true);
        Require(rdm.Stats().colorCoverageDraws==1 && rdm.Stats().maskedDraws==1,
            "partial color fixture did not exercise current-draw coverage");
        Require(reconstructed==native,"reconstruction overwrote outside actual color geometry");
    }
    params[2]=params[3]=0;ctx->UpdateSubresource(cb.Get(),0,nullptr,params,0,0);
    std::puts("RDM partial color coverage PASS: varying background, aligned/partial clusters and thin geometry");
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
    if(!nativeLazyDispatch)Require(Wrapped::acceptedDepth==10,"depth-prepass observers missed original draws");
    // Alpha/discard depth producers must not leave a plausible stale owner at
    // the same z. Invalidation affects their geometry, and a fresh prepass recovers.
    Prepare(true);ctx->Draw(3,0);CheckColor();
    Require(rdm.Stats().maskPreparations==1,"alpha invalidation fixture did not prepare a reusable mask");
    Bind(false);ctx->PSSetShader(alpha.Get(),nullptr,0);ctx->Draw(3,0);
    Require(rdm.Stats().guideInvalidationDraws==1,"alpha depth producer did not invalidate its geometry");
    const FLOAT clearedColor[4]{};
    ctx->ClearRenderTargetView(colorRT.Get(),clearedColor);ctx->ClearRenderTargetView(motionRT.Get(),clearedColor);
    Bind();Require(PrepareCoveredDraw(rdm,ctx.Get()),"guide invalidation unexpectedly disabled the whole renderer");
    Require(rdm.Stats().maskPreparations==2,"protected depth writer reused a stale prepared mask");
    ctx->Draw(3,0);rdm.AfterDraw(true);rdm.EndFrame();
    // The old prepass guide is invalid here, but the current color draw now
    // supplies its own coverage proof against final depth. It may be sparse;
    // the resolved image must still equal native rendering.
    Require(rdm.Stats().colorCoverageDraws > 0,"color draw reused invalid prepass ownership");
    CheckColor();
    for(unsigned mutation=0;mutation<(d24?3u:4u);++mutation) {
        Prepare(true);ctx->Draw(3,0);CheckColor();
        Require(rdm.Stats().maskPreparations==1,"depth-write mutation fixture did not create a cached mask");
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
        Require(rdm.Stats().maskPreparations==2,"depth-write mutation reused a stale prepared mask");
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
    if(!nativeLazyDispatch)Require(Wrapped::accepted>=8,"post-draw callback fixture did not run");
    if(info) {
        unsigned errors=0;
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> data(size);
            auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());HR(info->GetMessage(i,m,&size));
            if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) { std::printf("D3D11 WARNING/ERROR: %s\n",m->pDescription);++errors; }
        }
        Require(errors==0,"D3D11 debug layer reported errors");
    }
    Require(multithread->GetMultithreadProtected()==expectedProtection,"RDM changed game protection during rendering");
    std::printf("RDM HANDOFF PASS %ux%u %s auxiliaryFormat=%u debug=%u depthRange=%.9g/%.9g multithread=%d transition=%d submitted=%s\n",
        width,height,d24?"D24S8":"D32",unsigned(auxiliaryFormat),flags!=0,viewportMinDepth,viewportMaxDepth,
        expectedProtection,threading==ThreadingMode::EnableAfterInstall,
        separateSubmittedEyes?"separate eyes / depth ownership":"shared color");
}
int main(int argc,char** argv) {
    // Preserve the last successful stage even if a native detour/driver aborts
    // the process before C++ exception handling or redirected stdout can flush.
    std::setvbuf(stdout,nullptr,_IONBF,0);
    std::setvbuf(stderr,nullptr,_IONBF,0);
    try {
        bool nativeWarmDetours=false;
#ifdef OCU_RDM_TEST_DETOURS
        nativeWarmDetours=argc>1 && std::strcmp(argv[1],"--detours-native-warm-indexed")==0;
#endif
        if(argc>1 && (std::strcmp(argv[1],"--native-lazy-dispatch")==0 || nativeWarmDetours)) {
            // Each backend can run in a fresh process, before another fixture
            // populates the global hook cache. Never install a cloned table in
            // this mode: it would freeze D3D11's lazy dispatch and hide the bug.
            if(argc>2 && std::strcmp(argv[2],"--hardware")==0) {
                ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
                bool tested=false;
                for(UINT i=0;;++i) {
                    ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                    DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
                    std::printf("Native lazy-dispatch adapter %ls vendor=%04X\n",desc.Description,desc.VendorId);
                    Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,
                        0,0.999998f,ThreadingMode::Protected,InlineHookMode::None,true,nativeWarmDetours);tested=true;
                }
                Require(tested,"native lazy-dispatch hardware fixture found no hardware adapter");
            } else Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,
                0,0.999998f,ThreadingMode::Protected,InlineHookMode::None,true,nativeWarmDetours);
            std::puts("NATIVE LAZY-DISPATCH RDM HANDOFF TESTS PASS");return 0;
        }
#ifdef OCU_RDM_TEST_DETOURS
        if(argc>1 && (std::strcmp(argv[1],"--detours-early")==0 || std::strcmp(argv[1],"--detours-late")==0)) {
            const auto ordering=std::strcmp(argv[1],"--detours-early")==0?InlineHookMode::Early:InlineHookMode::Late;
            Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f,
                ThreadingMode::Protected,ordering);
            std::puts("RDM REAL DETOURS ORDER TEST PASS");return 0;
        }
#endif
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
        // D24 scenes can use MaxDepth=0.999998. Run the complete native
        // comparison/foreground/edge/partial-MRT suite with that range,
        // then with a shifted range to catch assumptions about a zero near bound.
        Run(nullptr,D3D_DRIVER_TYPE_WARP,134,70,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0.1f,0.8f);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,false,DXGI_FORMAT_R8G8B8A8_UNORM,true,0.1f,0.8f);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f,ThreadingMode::Protected);
        Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f,ThreadingMode::EnableAfterInstall);
        for(auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT})
            Run(nullptr,D3D_DRIVER_TYPE_WARP,128,64,true,format);
        ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(UINT i=0;;++i) {
            ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            std::printf("Adapter %ls vendor=%04X\n",desc.Description,desc.VendorId);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,false,DXGI_FORMAT_R8G8B8A8_UNORM,true,0.1f,0.8f);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f,ThreadingMode::Protected);
            Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,128,64,true,DXGI_FORMAT_R8G8B8A8_UNORM,true,0,0.999998f,ThreadingMode::EnableAfterInstall);
        }
        std::puts("ALL RDM HANDOFF TESTS PASS");return 0;
    }catch(const std::exception& e) {std::printf("FAIL: %s\n",e.what());return 1;}
}
