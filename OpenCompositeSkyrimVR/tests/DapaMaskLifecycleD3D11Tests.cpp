// Executes extracted production SKSE Prepare/Publish and real-frame boundary,
// with the exact production GPU mask renderer. No Skyrim/OpenXR initialization.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <stdexcept>
#include "DrvOpenXR/DapaPlayerMaskGpu.h"
#include "DrvOpenXR/DapaMaskBridge.h"
#include "OpenOVR/Compositor/RDMDiagnosticSchedule.h"
using Microsoft::WRL::ComPtr;
namespace SKSE::log { template<class... Args> void debug(const char*, Args...) {} }
static unsigned checks=0;
static void Check(bool pass,const char* reason) { ++checks;if(!pass) throw std::runtime_error(reason); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr),"D3D11 call failed"); }
struct Bridge {
    unsigned char _padPreFP[3]{DapaMaskBridge::Format};
    unsigned char preFPDepthCaptured=0;
    uint64_t preFPDepthTexture=0;
    std::uint32_t maskAccessGate=0,maskFrameGeneration=0,maskConflictSerial=0,maskFrameConflictBaseline=0;
};
using MaskAccess=DapaMaskBridge::Access<Bridge>;
struct BridgePointer {
    Bridge* value=nullptr;
    Bridge* Get() const { return value; }
    Bridge* operator->() const { return value; }
    operator Bridge*() const { return value; }
    BridgePointer& operator=(Bridge* pointer) { value=pointer;return *this; }
};
static Bridge bridge;
static Bridge* g_pBridge=&bridge;
static BridgePointer s_pBridge{&bridge};
static RDMDiagnosticSchedule rdmDiagnosticSchedule;
static Bridge* pendingBridge=nullptr;
static unsigned bridgeLookups=0;
static int OCBridge_MenuState() {
    ++bridgeLookups;
    if(!s_pBridge && pendingBridge)s_pBridge=pendingBridge;
    return -1;
}
static std::atomic<bool> g_diagnosticLogging=false,ready=true;
static bool needsClear=true;
static DapaPlayerMaskGpu gpu;
static ID3D11DeviceContext* immediate=nullptr;
struct Resources { ID3D11Texture2D* depthTexture=nullptr; ID3D11Device* d3dDevice=nullptr; };
static Resources resources;
static Resources AcquirePublishedBridgeResources() { return resources; }
static uint64_t ownedCallbacks=0,draws=0,classified=0,higgsClassified=0,spellWheelClassified=0,vrArrowClassified=0,crossbowClassified=0,vrEquipmentClassified=0;
static std::array<uint64_t,8> rejected{};
static std::chrono::steady_clock::time_point lastLog{};
#include "DapaMaskLifecycleProduction.inc"

int main() try {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    immediate=context.Get();resources.d3dDevice=device.Get();
    constexpr UINT width=16,height=8;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R32_TYPELESS;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11DepthStencilView> dsv;
    HR(device->CreateTexture2D(&desc,nullptr,&depth));resources.depthTexture=depth.Get();
    D3D11_DEPTH_STENCIL_VIEW_DESC view{};view.Format=DXGI_FORMAT_D32_FLOAT;view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    HR(device->CreateDepthStencilView(depth.Get(),&view,&dsv));
    const char* code="float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0.4,1);}";
    ComPtr<ID3DBlob> bytes;HR(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&bytes,nullptr));
    ComPtr<ID3D11VertexShader> vs;HR(device->CreateVertexShader(bytes->GetBufferPointer(),bytes->GetBufferSize(),nullptr,&vs));
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(device->CreateRasterizerState(&rasterDesc,&raster));context->RSSetState(raster.Get());
    context->VSSetShader(vs.Get(),nullptr,0);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetRenderTargets(0,nullptr,dsv.Get());context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,.4f,0);
    Check(gpu.Initialize(device.Get()),"production GPU mask initialized");
    auto ownedDraw=[&](bool right){
        D3D11_VIEWPORT viewport{right?8.f:0.f,0,8,8,0,1};context->RSSetViewports(1,&viewport);
        MaskAccess access;
        Check(Prepare(context.Get(),true,access),"production player draw preparation");
        Check(gpu.Replay(context.Get(),[&]{context->Draw(3,0);}),"production player mask replay");
        Check(Publish(access),"production mask publication under write lease");
    };
    auto stalePixels=[&](){
        D3D11_TEXTURE2D_DESC d{};gpu.Texture()->GetDesc(&d);d.BindFlags=0;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> readback;HR(device->CreateTexture2D(&d,nullptr,&readback));context->CopyResource(readback.Get(),gpu.Texture());
        D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));unsigned stale=0,fresh=0;
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){float v;std::memcpy(&v,static_cast<char*>(mapped.pData)+y*mapped.RowPitch+x*4,4);
            if(v>=0&&v<=1&&std::abs(v-.4f)<=.000002f){if(x<8)++stale;else++fresh;}}
        context->Unmap(readback.Get(),0);Check(fresh==64,"current-frame player coverage survives");return stale;
    };
    BeginRealFrame();ownedDraw(false); // real frame A; DAPA does not cache it
    BeginRealFrame();ownedDraw(true);  // frame B moves the player geometry
    unsigned stale=stalePixels();std::printf("UNCACHED_FRAME staleDepthValidatedPixels=%u expected=0\n",stale);
    Check(stale==0,"uncached real-frame turnover retained stale player mask pixels");
    for(int frame=0;frame<20;++frame){BeginRealFrame();ownedDraw(frame%2!=0);}
    Check(stalePixels()==0,"repeated uncached frames accumulate no stale mask");
    BeginRealFrame();Check(!bridge.preFPDepthCaptured,"frame with no player draws cannot reuse prior mask");
    // Unsupported peers cannot expose their raw pointer through the new lease.
    bridge._padPreFP[0]=1;bridge.preFPDepthCaptured=1;BeginRealFrame();
    {MaskAccess unsupported(&bridge,DapaMaskBridge::AccessMode::Read);
        Check(!unsupported && !unsupported.Valid() && !unsupported.Texture(),"legacy format is rejected without reading its texture");}
    bridge._padPreFP[0]=DapaMaskBridge::Format;BeginRealFrame();
    Check(!bridge.preFPDepthCaptured,"restored protocol expires previous publication");

    BeginRealFrame();ownedDraw(false);
    {
        MaskAccess read(&bridge,DapaMaskBridge::AccessMode::Read);
        Check(read.Valid(),"current mask acquired by compositor read lease");
        auto* before=gpu.Texture();
        const auto oldConflict=bridge.maskConflictSerial;
        MaskAccess nonplayer;
        Check(!Prepare(context.Get(),false,nonplayer) && read.Clean(),"non-player rejection does not poison active mask reader");
        MaskAccess wrongContext;
        Check(!Prepare(nullptr,true,wrongContext) && read.Clean(),"wrong-context draw is rejected before mask lease");
        MaskAccess denied;
        Check(!Prepare(context.Get(),true,denied),"producer cannot resize or clear during a compositor mask read");
        Check(gpu.Texture()==before,"denied producer leaves texture lifetime unchanged");
        Check(bridge.maskConflictSerial==oldConflict+1 && !read.Valid(),"missed owned draw poisons partial coverage for this frame");
    }
    {MaskAccess read(&bridge,DapaMaskBridge::AccessMode::Read);Check(!read.Valid(),"poisoned frame cannot become readable after gate release");}
    BeginRealFrame();ownedDraw(true);Check(stalePixels()==0,"clean next frame recovers after rejected overlapping producer");
    {
        MaskAccess write;
        Check(Prepare(context.Get(),true,write),"writer holds lease across replay and publication");
        BeginRealFrame();
        Check(!write.Clean() && !Publish(write),"frame boundary collision prevents old-generation publication");
    }
    {MaskAccess read(&bridge,DapaMaskBridge::AccessMode::Read);Check(!read.Valid(),"boundary collision cannot expose stale coverage");}
    BeginRealFrame();ownedDraw(true);Check(stalePixels()==0,"next real frame recovers after boundary collision");
    {
        MaskAccess write;
        Check(Prepare(context.Get(),true,write),"prepare replay-failure scenario");
        write.Poison();
        Check(!Publish(write),"failed replay cannot publish a valid mask");
    }
    {MaskAccess read(&bridge,DapaMaskBridge::AccessMode::Read);Check(!read.Valid(),"replay-failure frame is rejected by consumer");}
    BeginRealFrame();ownedDraw(true);Check(stalePixels()==0,"new frame clears pixels after failed replay");
    s_pBridge=nullptr;BeginRealFrame();Check(!s_pBridge,"absent bridge stays absent");
    bridge.preFPDepthCaptured=1;pendingBridge=&bridge;
    const auto beforeLookups=bridgeLookups;BeginRealFrame();
    Check(s_pBridge==&bridge&&!bridge.preFPDepthCaptured,"bridge connecting at this boundary expires earlier mask immediately");
    Check(bridgeLookups==beforeLookups+1,"frame reset reuses one existing bridge lookup");
    std::printf("PASS: %u production/GPU lifecycle checks; uncached-frame pixels, lease conflicts, failed replay, legacy rejection and bridge recovery\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
