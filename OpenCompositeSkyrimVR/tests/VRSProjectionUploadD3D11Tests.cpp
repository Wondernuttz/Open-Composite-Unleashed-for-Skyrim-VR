// Actual NVIDIA atlas uploads; private access is confined to failure injection
// and texture inspection in this fixture, with no runtime testing API.
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <vector>
#include <array>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <stdexcept>
#include "OpenOVR/Compositor/VRSSceneScope.h"
#include "OpenOVR/Misc/FoveationRates.h"
#define private public
#include "OpenOVR/Compositor/VRSManager.h"
#undef private
// Compile the implementation in this TU too: MSVC encodes method access in
// symbols, so linking to a separately compiled private declaration is invalid.
#include "OpenOVR/Compositor/VRSManager.cpp"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*,long,const char*,const char* text) { std::puts(text); }
void oovr_log_raw_format(const char*,long,const char*,const char* format,...) {
    va_list args; va_start(args,format); std::vprintf(format,args); va_end(args); std::puts("");
}
static unsigned checks=0;
void Check(bool value,const char* reason) { ++checks; if(!value) throw std::runtime_error(reason); }
void HR(HRESULT value) { Check(SUCCEEDED(value),"D3D11 call failed"); }
void VerifyAtlas(VRSManager& manager) {
    D3D11_TEXTURE2D_DESC desc{}; manager.vrsTex->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback; HR(manager.device->CreateTexture2D(&desc,nullptr,&readback));
    manager.context->CopyResource(readback.Get(),manager.vrsTex);
    D3D11_MAPPED_SUBRESOURCE map{}; HR(manager.context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map));
    unsigned wrong=0;
    for(UINT y=0;y<desc.Height;++y) for(UINT x=0;x<desc.Width;++x) {
        const unsigned eye=x<desc.Width/2?0:1;
        const float u=(float(x%(desc.Width/2))+.5f)/float(desc.Width/2);
        const float v=(float(y)+.5f)/float(desc.Height);
        const float dx=(u-manager.uploadedProjX[eye])/manager.horizontalScale,dy=v-manager.uploadedProjY[eye];
        const float distance=2.0f*std::sqrt(dx*dx+dy*dy);
        const unsigned expected=distance<manager.cachedInnerRadius?1:distance<manager.cachedMidRadius?2:3;
        const auto actual=static_cast<const unsigned char*>(map.pData)[y*map.RowPitch+x];
        wrong+=actual!=expected;
    }
    manager.context->Unmap(readback.Get(),0);
    Check(wrong==0,"GPU atlas differs from last uploaded eye centers");
}
int main() {
 try {
    ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if(desc.VendorId!=0x10de) continue;
        if(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context))) { std::wprintf(L"Adapter: %ls\n",desc.Description); break; }
    }
    Check(device!=nullptr,"NVIDIA hardware required");
    VRSManager manager; Check(manager.Initialize(device.Get()),"VRS initialization required");
    const VRSManager::EyeRegion left{0,0,4096,4096},right{4096,0,4096,4096};
    const ocu_foveation::RingRates rates{ocu_foveation::Rate::X1x1,
        ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X4x4};
    const auto update=[&]() { return manager.UpdateStereoPattern(8192,4096,left,right,.3f,.6f,rates); };
    manager.SetProjectionCenters(.5f,.5f,.5f,.5f); Check(update(),"initial pattern"); VerifyAtlas(manager);
    for(float scale:{.5f,1.5f,2.f,1.f}) {
        const auto shapeBefore=manager.GetPatternUpdates();
        manager.SetHorizontalScale(scale);Check(update(),"shape update");VerifyAtlas(manager);
        Check(manager.GetPatternUpdates().uploads==shapeBefore.uploads+1 &&
            manager.GetPatternUpdates().resourceCreations==shapeBefore.resourceCreations,"shape change uploads without resource recreation");
        const auto stable=manager.GetPatternUpdates();
        manager.SetHorizontalScale(scale);Check(update(),"unchanged shape");
        Check(manager.GetPatternUpdates().uploads==stable.uploads,"unchanged shape does not upload again");
    }
    auto before=manager.GetPatternUpdates();
    for(unsigned frame=0;frame<1000;++frame) {
        manager.SetProjectionCenters(.5f,.5f,.5f,.5f); Check(update(),"stationary update");
    }
    Check(manager.GetPatternUpdates().uploads==before.uploads,"stationary centers upload nothing");
    std::puts("STATIONARY frames=1000 uploads=0");
    // All four coordinates move in individually sub-threshold increments.
    float maxError=0;
    before=manager.GetPatternUpdates();
    for(unsigned frame=1;frame<=2000;++frame) {
        const float offset=float(frame)*.00005f;
        manager.SetProjectionCenters(.5f+offset,.5f-offset,.5f-offset,.5f+offset);
        Check(update(),"tiny movement update");
        for(unsigned eye=0;eye<2;++eye) {
            maxError=std::max(maxError,std::fabs(manager.projX[eye]-manager.uploadedProjX[eye]));
            maxError=std::max(maxError,std::fabs(manager.projY[eye]-manager.uploadedProjY[eye]));
        }
        Check(maxError<=.0001f,"cumulative unpublished drift is bounded");
    }
    auto after=manager.GetPatternUpdates();
    Check(after.uploads>before.uploads,"tiny movement eventually uploads");
    Check(after.resourceCreations==before.resourceCreations,"moving eyes reuse rate resource");
    VerifyAtlas(manager);
    std::printf("TINY frames=2000 uploads=%u creations=0 maxUnpublishedUV=%.9f maxPixelsAt4096=%.6f\n",
        after.uploads-before.uploads,maxError,maxError*4096.f);
    before=after;
    for(unsigned frame=0;frame<120;++frame) {
        const float offset=float(frame)*.001f;
        manager.SetProjectionCenters(.4f+offset,.6f,.6f,.4f+offset); Check(update(),"normal movement update");
    }
    after=manager.GetPatternUpdates();
    Check(after.uploads-before.uploads==120,"ordinary movement uploads once per update");
    Check(after.resourceCreations==before.resourceCreations,"ordinary movement reuses resource"); VerifyAtlas(manager);
    std::printf("NORMAL frames=120 uploads=%u creations=0\n",after.uploads-before.uploads);
    // Missing upload prerequisites must not consume the pending position.
    auto* savedContext=manager.context; manager.context=nullptr;
    manager.SetProjectionCenters(.3f,.4f,.7f,.6f);
    const auto previousUploaded=manager.uploadedProjX[0];
    Check(!update(),"withheld upload remains dirty");
    Check(manager.patternDirty && manager.uploadedProjX[0]==previousUploaded,"failed prerequisite preserves baseline");
    manager.SetProjectionCenters(.31f,.41f,.69f,.59f);
    manager.context=savedContext; Check(update(),"retry latest centers");
    Check(manager.uploadedProjX[0]==.31f && manager.uploadedProjY[1]==.59f,"retry publishes latest sample"); VerifyAtlas(manager);
    // Resource/device invalidation requires a fresh publication even at the same centers.
    before=manager.GetPatternUpdates(); manager.ReleasePatternResources();
    Check(manager.patternDirty,"release invalidates uploaded pattern"); Check(update(),"resource recreation");
    after=manager.GetPatternUpdates();
    Check(after.uploads==before.uploads+1 && after.resourceCreations==before.resourceCreations+1,"recreation uploads once"); VerifyAtlas(manager);
    manager.Shutdown(); Check(manager.patternDirty && !manager.vrsTex,"shutdown invalidates pattern");
    // Existing initialization policy requires a new manager for a new device session.
    VRSManager nextSession; Check(nextSession.Initialize(device.Get()),"new session manager");
    nextSession.SetProjectionCenters(.31f,.41f,.69f,.59f);
    Check(nextSession.UpdateStereoPattern(8192,4096,left,right,.3f,.6f,rates),"new session uploads current centers"); VerifyAtlas(nextSession);
    std::printf("PASS: %u production projection/upload checks\n",checks); return 0;
 } catch(const std::exception& error) { std::printf("FAIL: %s\n",error.what()); return 1; }
}
