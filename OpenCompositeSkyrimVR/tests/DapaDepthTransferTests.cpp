#include "DrvOpenXR/DapaDepthTransfer.h"
#include <d3d11sdklayers.h>
#include <vector>
#include <cstdio>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void Check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr),"D3D11 failure"); }
static void Run(D3D_DRIVER_TYPE driver)
{
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    HR(D3D11CreateDevice(nullptr,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11InfoQueue> debug; HR(device.As(&debug));
    DapaDepthTransfer transfer;
    for (UINT width : {8u,13u,8u}) for(auto format : {DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_R32_FLOAT}) {
        constexpr UINT height=7;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=width; desc.Height=height;
        desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R32_FLOAT;
        std::vector<float> values[2]; ComPtr<ID3D11Texture2D> eyes[2];
        for(int eye=0;eye<2;++eye) {
            values[eye].resize(width*height);
            for(UINT i=0;i<width*height;++i) values[eye][i]=float(i+1+eye*100)/1000;
            D3D11_SUBRESOURCE_DATA initial{values[eye].data(),width*4,0};
            HR(device->CreateTexture2D(&desc,&initial,&eyes[eye]));
        }
        desc.Width=width*2; desc.Format=format;
        desc.BindFlags=format==DXGI_FORMAT_D32_FLOAT ? D3D11_BIND_DEPTH_STENCIL : 0;
        ComPtr<ID3D11Texture2D> target; HR(device->CreateTexture2D(&desc,nullptr,&target));
        Check(transfer.Copy(context.Get(),target.Get(),eyes[0].Get(),eyes[1].Get()),"depth atlas transfer rejected");
        Check(!transfer.Copy(context.Get(),eyes[0].Get(),eyes[0].Get(),eyes[1].Get()),"wrong atlas size must be rejected");
        Check(!transfer.Copy(context.Get(),target.Get(),nullptr,eyes[1].Get()),"missing eye must be rejected");
        desc.Format=DXGI_FORMAT_R32_FLOAT; desc.BindFlags=0;
        desc.Usage=D3D11_USAGE_STAGING; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> read; HR(device->CreateTexture2D(&desc,nullptr,&read));
        context->CopyResource(read.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE map{}; HR(context->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
        for(UINT y=0;y<height;++y) for(UINT x=0;x<width*2;++x) {
            float actual; std::memcpy(&actual,static_cast<char*>(map.pData)+y*map.RowPitch+x*4,4);
            Check(actual==values[x/width][y*width+x%width],"depth bits or stereo eye placement changed");
        }
        context->Unmap(read.Get(),0);
    }
    for(UINT64 i=0;i<debug->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0; HR(debug->GetMessage(i,nullptr,&size));
        std::vector<unsigned char> data(size); auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());
        HR(debug->GetMessage(i,message,&size));
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
            std::printf("D3D11: %s\n",message->pDescription);
            Check(false,"D3D11 validation warning/error");
        }
    }
    transfer.Reset(); context->ClearState(); context->Flush();
    std::printf("PASS %s D32/R32 depth: both eyes exact, resize/reuse, no D3D11 warnings/errors\n",driver==D3D_DRIVER_TYPE_WARP?"WARP":"hardware");
}
int main() {
    try {Run(D3D_DRIVER_TYPE_WARP);Run(D3D_DRIVER_TYPE_HARDWARE);return 0;}
    catch(const std::exception& e) {std::printf("FAIL: %s\n",e.what());return 1;}
}
