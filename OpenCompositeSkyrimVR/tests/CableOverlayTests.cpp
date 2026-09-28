#include <d3d11.h>
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include "DrvOpenXR/CableTrackingOverlay.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
constexpr unsigned Width=640, Height=192;

static unsigned checks = 0;
static void Check(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT value) { Check(SUCCEEDED(value), "D3D11 call failed"); }
static ID3D11Device* mockDevice;
static ComPtr<ID3D11Texture2D> mockImage;
static bool pendingAcquire = false, waited = false;
static XrResult nextWait = XR_SUCCESS;
static int64_t offeredFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
static unsigned acquires = 0, waits = 0, releases = 0, destroys = 0, formatQueries = 0;
static constexpr auto ChainValue = uintptr_t(1);

extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, uint32_t capacity, uint32_t* count, int64_t* formats)
{
    ++formatQueries; *count = 1; if (capacity) formats[0] = offeredFormat; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output)
{
    Check(info->width == Width && info->height == Height && info->arraySize == 1, "single mono overlay image dimensions");
    Check(info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT, "upload usage declared");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = info->width; desc.Height = info->height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = static_cast<DXGI_FORMAT>(info->format);
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HR(mockDevice->CreateTexture2D(&desc, nullptr, mockImage.ReleaseAndGetAddressOf()));
    *output = reinterpret_cast<XrSwapchain>(ChainValue);
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain)
{
    ++destroys; mockImage.Reset(); pendingAcquire = waited = false; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images)
{
    *count = 1;
    if (capacity) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture = mockImage.Get();
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{
    Check(!pendingAcquire, "never reacquire a timed-out image");
    ++acquires; pendingAcquire = true; waited = false; *index = 0; return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo* info)
{
    Check(pendingAcquire && !waited && info->timeout == 0, "correct nonblocking wait order");
    ++waits; waited = nextWait == XR_SUCCESS; return nextWait;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*)
{
    Check(pendingAcquire && waited, "never release before successful wait");
    ++releases; pendingAcquire = waited = false; return XR_SUCCESS;
}
}


extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* c, XrSpace* s) {
 Check(c->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL,"physical LOCAL reference independent of app locomotion");
 *s=reinterpret_cast<XrSpace>(2);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) { return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace,XrSpace,XrTime,XrSpaceLocation* l) {
 l->locationFlags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
 l->pose.orientation={0,0,0,1};return XR_SUCCESS;
}
}
int main() {
 try {
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
  mockDevice=device.Get();
  for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
   offeredFormat=format;
   unsigned startDestroy=destroys;
   {
    CableTrackingOverlay overlay;
    ocu_cable::Settings config;config.enabled=true;config.showKey=config.resetKey=0;config.warningTurns=0;
    auto session=reinterpret_cast<XrSession>(1);auto view=reinterpret_cast<XrSpace>(3);
    auto Update=[&](bool room=true,bool focus=true){return overlay.Update(session,view,1000000000,focus,room,device.Get(),config);};
    const auto start=acquires;
    Check(Update()==nullptr&&acquires==start,"hidden counter has no GPU upload or layer");
    ocu_cable::Shortcut(ocu_cable::ShowAction,false);
    Check(Update(false)==nullptr&&acquires==start,"layer budget exhaustion skips overlay");
    nextWait=XR_TIMEOUT_EXPIRED;
    Check(Update()==nullptr&&acquires==start+1,"busy image skipped without blocking");
    nextWait=XR_SUCCESS;
    auto* header=Update();Check(header!=nullptr&&acquires==start+1,"retry same acquired image");
    auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(header);
    Check(quad->type==XR_TYPE_COMPOSITION_LAYER_QUAD&&quad->eyeVisibility==XR_EYE_VISIBILITY_BOTH,"one quad visible to both eyes");
    Check(quad->space==view&&quad->pose.position.z<0,"head-relative overlay in front of headset");
    auto released=releases;
    Check(Update()!=nullptr&&releases==released,"unchanged overlay reuses image without upload");
    Check(Update(true,false)==nullptr,"overlay hidden when session lacks focus");
    ocu_cable::Shortcut(ocu_cable::ResetAction,false);Check(Update()!=nullptr&&releases==released+1,"reset updates display immediately");
    D3D11_TEXTURE2D_DESC desc{};mockImage->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> read;HR(device->CreateTexture2D(&desc,nullptr,&read));context->CopyResource(read.Get(),mockImage.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    std::ofstream out(format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?"cable-bgra.raw":"cable-rgba.raw",std::ios::binary);
    unsigned green=0;
    for(unsigned y=0;y<Height;++y){auto* row=(unsigned char*)mapped.pData+y*mapped.RowPitch;out.write((char*)row,Width*4);
     for(unsigned x=0;x<Width;++x) {auto* p=row+x*4;Check(p[3]==255,"opaque panel alpha");if(p[1]>200&&p[1]>p[0]&&p[1]>p[2])++green;}}
    context->Unmap(read.Get(),0);Check(green>500,"actual uploaded image contains neon-green text");
   }
   Check(destroys==startDestroy+1,"session cleanup releases overlay swapchain");
  }
  printf("Cable overlay: %u checks passed (D3D11 WARP + mocked OpenXR)\n",checks);return 0;
 }catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
