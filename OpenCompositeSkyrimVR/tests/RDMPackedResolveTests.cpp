#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <stdexcept>
#include "OpenOVR/Compositor/DensityMaskManager.h"
using Microsoft::WRL::ComPtr;
void oovr_log_raw(const char*,long,const char*,const char* message){std::puts(message);}
void oovr_log_raw_format(const char*,long,const char*,const char* format,...){va_list a;va_start(a,format);std::vprintf(format,a);va_end(a);std::puts("");}
static void Require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void HR(HRESULT hr){if(FAILED(hr)){std::printf("HRESULT=%08X\n",unsigned(hr));throw std::runtime_error("D3D11 failure");}}
static std::vector<unsigned char> Read(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11Texture2D* source,UINT stride){
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;HR(d->CreateTexture2D(&desc,nullptr,&staging));c->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE map{};HR(c->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
    std::vector<unsigned char> bytes(size_t(desc.Width)*desc.Height*stride);
    for(UINT y=0;y<desc.Height;y++)std::memcpy(bytes.data()+size_t(y)*desc.Width*stride,static_cast<char*>(map.pData)+size_t(y)*map.RowPitch,size_t(desc.Width)*stride);
    c->Unmap(staging.Get(),0);return bytes;
}
static void Run(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver){
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level;
    HR(D3D11CreateDevice(adapter,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
    ComPtr<ID3D11InfoQueue> debug;HR(d.As(&debug));
    constexpr UINT width=250,height=73;
    DensityMaskManager legacy,packed;Require(legacy.Initialize(d.Get())&&packed.Initialize(d.Get()),"manager init");
    std::array<ComPtr<ID3D11Texture2D>,8> source;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    for(auto& t:source)HR(d->CreateTexture2D(&desc,nullptr,&t));
    ComPtr<ID3D11Texture2D> reference,privateDepth,coverage;
    HR(d->CreateTexture2D(&desc,nullptr,&reference));
    ComPtr<ID3D11ShaderResourceView> sentinel;HR(d->CreateShaderResourceView(reference.Get(),nullptr,&sentinel));
    desc.Format=DXGI_FORMAT_R8_UNORM;HR(d->CreateTexture2D(&desc,nullptr,&coverage));
    ComPtr<ID3D11RenderTargetView> coverageRT;ComPtr<ID3D11ShaderResourceView> coverageSRV;
    HR(d->CreateRenderTargetView(coverage.Get(),nullptr,&coverageRT));HR(d->CreateShaderResourceView(coverage.Get(),nullptr,&coverageSRV));
    desc.Format=DXGI_FORMAT_D32_FLOAT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    HR(d->CreateTexture2D(&desc,nullptr,&privateDepth));ComPtr<ID3D11DepthStencilView> privateDSV;
    HR(d->CreateDepthStencilView(privateDepth.Get(),nullptr,&privateDSV));
    const DensityMaskManager::EyeRegion eyes[]={{3,2,107,67},{117,1,129,70}};
    std::vector<DensityMaskManager::PatternSettings> patterns;
    patterns.push_back({.23f,.57f,true,false,{}});patterns.push_back({.23f,.57f,false,false,{}});
    for(unsigned rate=0;rate<7;rate++)patterns.push_back({.23f,.57f,false,true,
        {ocu_foveation::Rate(rate),ocu_foveation::Rate((rate+2)%7),ocu_foveation::Rate((rate+4)%7)}});
    for(unsigned patternIndex=0;patternIndex<patterns.size();patternIndex++)for(unsigned slots:{0xffu,0x89u,0x02u}) {
        const auto& pattern=patterns[patternIndex];legacy.SetPatternSettings(pattern);packed.SetPatternSettings(pattern);
        legacy.SetProjectionCenters(.12f,.83f,.89f,.19f);packed.SetProjectionCenters(.12f,.83f,.89f,.19f);
        Require(legacy.PrepareStereoTarget(reference.Get(),width,height,eyes[0],eyes[1]),"legacy geometry");
        const float zero[4]{};c->ClearRenderTargetView(coverageRT.Get(),zero);c->ClearDepthStencilView(privateDSV.Get(),D3D11_CLEAR_DEPTH,1,0);
        Require(legacy.ApplyDepthMask(privateDSV.Get(),1,coverageRT.Get()),"coverage mask");
        auto holes=Read(d.Get(),c.Get(),coverage.Get(),1);
        std::array<std::vector<unsigned char>,8> expected;
        ID3D11Texture2D* targets[8]{};
        for(unsigned slot=0;slot<8;slot++)if(slots&(1u<<slot)) {
            targets[slot]=source[slot].Get();std::vector<unsigned char> pixels(size_t(width)*height*4);
            for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++) {
                size_t i=size_t(y)*width+x;
                for(unsigned channel=0;channel<4;channel++)pixels[i*4+channel]=holes[i]?0:static_cast<unsigned char>((x*13+y*29+channel*61+slot*19)%256);
            }
            c->UpdateSubresource(source[slot].Get(),0,nullptr,pixels.data(),width*4,0);
            c->UpdateSubresource(reference.Get(),0,nullptr,pixels.data(),width*4,0);
            Require(legacy.ResolveColor(reference.Get(),coverageSRV.Get(),3),"legacy reference resolve");
            expected[slot]=Read(d.Get(),c.Get(),reference.Get(),4);
        }
        Require(packed.PrepareMRTTargets(targets,8,width,height,eyes[0],eyes[1]),"prepare packed MRTs");
        ID3D11ShaderResourceView* sentinels[9];for(auto*& srv:sentinels)srv=sentinel.Get();c->PSSetShaderResources(0,9,sentinels);
        Require(packed.ResolveMRTs(coverageSRV.Get(),3),"packed resolve");
        ID3D11ShaderResourceView* restored[9]{};c->PSGetShaderResources(0,9,restored);
        for(auto* srv:restored){Require(srv==sentinel.Get(),"pixel SRV state not restored");srv->Release();}
        ID3D11ShaderResourceView* none[9]{};c->PSSetShaderResources(0,9,none);
        for(unsigned slot=0;slot<8;slot++)if(slots&(1u<<slot)) {
            auto result=Read(d.Get(),c.Get(),source[slot].Get(),4);
            if(result!=expected[slot])std::printf("pattern=%u targetMask=%02X slot=%u\n",patternIndex,slots,slot);
            Require(result==expected[slot],"packed samples differ from established reconstruction mapping");
        }
    }
    UINT warnings=0;
    for(UINT64 i=0;i<debug->GetNumStoredMessages();i++) {
        SIZE_T size=0;debug->GetMessage(i,nullptr,&size);std::vector<char> data(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());HR(debug->GetMessage(i,message,&size));
        if(message->Severity<=D3D11_MESSAGE_SEVERITY_WARNING){std::puts(message->pDescription);warnings++;}
    }
    Require(warnings==0,"D3D11 warnings/errors");
    std::puts("PACKED MRT PASS: all rates, checker/quarter/sixteenth, MRT gaps, asymmetric eyes, gutters, partial tiles, PS t0-t8 restoration");
}
int main(){try{
    Run(nullptr,D3D_DRIVER_TYPE_WARP);
    ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;HR(factory->EnumAdapters1(0,&adapter));
    Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN);return 0;
}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}}
