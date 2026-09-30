#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "../OpenOVR/Compositor/ExactPixelShader.h"

// Render-thread only. Replay just the current draw's geometry into raw depth.
// No game colour/depth writes, shader replacement in the game, or CPU readback.
class DapaPlayerMaskGpu {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Texture2D> texture;
    Ptr<ID3D11RenderTargetView> target;
    Ptr<ID3D11PixelShader> shader;
    Ptr<ID3D11PixelShader> visibleDepthShader;
    Ptr<ID3D11DepthStencilState> noDepth;
    Ptr<ID3D11DepthStencilState> equalDepth;
    Ptr<ID3D11BlendState> noBlend;
    Ptr<ID3D11DepthStencilView> sourceView,readOnlyDepth;
    Ptr<ID3D11ShaderResourceView> sourceDepth;
    UINT width=0,height=0,uavSlots=8;
public:
    ID3D11Texture2D* Texture() const { return texture.Get(); }
    bool Initialize(ID3D11Device* device) {
        uavSlots=device->GetFeatureLevel()>=D3D_FEATURE_LEVEL_11_1?64:8;
        static constexpr char code[] = "float main(float4 p:SV_Position):SV_Target { return p.z; }";
        Ptr<ID3DBlob> blob,errors;
        if(FAILED(D3DCompile(code,sizeof(code)-1,"DapaBodyMask",nullptr,nullptr,"main","ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors)))return false;
        if(FAILED(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader)))return false;
        if(!ocu_exact_pixels::Mark(shader.Get()))return false;
        static constexpr char visibleCode[] = "Texture2D<float> sceneDepth:register(t0); float main(float4 p:SV_Position):SV_Target { return sceneDepth.Load(int3(int2(p.xy),0)); }";
        if(FAILED(D3DCompile(visibleCode,sizeof(visibleCode)-1,"DapaVisibleBodyMask",nullptr,nullptr,"main","ps_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&errors)))return false;
        if(FAILED(device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&visibleDepthShader)))return false;
        // These values are compared against scene depth within 2e-6 by DAPA.
        // Coarse VRS would broadcast one pixel's depth across neighboring pixels.
        if(!ocu_exact_pixels::Mark(visibleDepthShader.Get()))return false;
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable=FALSE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        if(FAILED(device->CreateDepthStencilState(&ds,&noDepth)))return false;
        ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_EQUAL;
        if(FAILED(device->CreateDepthStencilState(&ds,&equalDepth)))return false;
        D3D11_BLEND_DESC blend{};blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED;
        return SUCCEEDED(device->CreateBlendState(&blend,&noBlend));
    }
    bool Size(ID3D11Device* device,UINT w,UINT h) {
        if(texture && w==width && h==height)return true;
        Ptr<ID3D11Texture2D> next;Ptr<ID3D11RenderTargetView> view;
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;
        d.Format=DXGI_FORMAT_R32_FLOAT;d.SampleDesc.Count=1;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateTexture2D(&d,nullptr,&next)) ||
            FAILED(device->CreateRenderTargetView(next.Get(),nullptr,&view)))return false;
        texture=next;target=view;width=w;height=h;return true;
    }
    void Clear(ID3D11DeviceContext* ctx) {
        const float empty[4]={-1,-1,-1,-1};ctx->ClearRenderTargetView(target.Get(),empty);
    }
    bool DepthViews(ID3D11DepthStencilView* depth) {
        if(sourceView.Get()==depth && readOnlyDepth && sourceDepth)return true;
        Ptr<ID3D11Device> device;depth->GetDevice(&device);
        Ptr<ID3D11Resource> resource;depth->GetResource(&resource);
        D3D11_DEPTH_STENCIL_VIEW_DESC vd{};depth->GetDesc(&vd);
        if(vd.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip=vd.Texture2D.MipSlice;sd.Texture2D.MipLevels=1;
        vd.Flags|=D3D11_DSV_READ_ONLY_DEPTH;
        switch(vd.Format) {
            case DXGI_FORMAT_D24_UNORM_S8_UINT:sd.Format=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;vd.Flags|=D3D11_DSV_READ_ONLY_STENCIL;break;
            case DXGI_FORMAT_D32_FLOAT:sd.Format=DXGI_FORMAT_R32_FLOAT;break;
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:sd.Format=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;vd.Flags|=D3D11_DSV_READ_ONLY_STENCIL;break;
            case DXGI_FORMAT_D16_UNORM:sd.Format=DXGI_FORMAT_R16_UNORM;break;
            default:return false;
        }
        if(readOnlyDepth && sourceDepth) {
            Ptr<ID3D11Resource> cached;readOnlyDepth->GetResource(&cached);
            D3D11_DEPTH_STENCIL_VIEW_DESC previous{};readOnlyDepth->GetDesc(&previous);
            if(cached.Get()==resource.Get() && previous.Format==vd.Format &&
               previous.Texture2D.MipSlice==vd.Texture2D.MipSlice) {sourceView=depth;return true;}
        }
        Ptr<ID3D11DepthStencilView> nextView;Ptr<ID3D11ShaderResourceView> nextSource;
        if(FAILED(device->CreateDepthStencilView(resource.Get(),&vd,&nextView)) ||
           FAILED(device->CreateShaderResourceView(resource.Get(),&sd,&nextSource)))return false;
        sourceView=depth;readOnlyDepth=nextView;sourceDepth=nextSource;return true;
    }
    template<class Draw> bool Replay(ID3D11DeviceContext* ctx,Draw&& draw) {
        Ptr<ID3D11DepthStencilView> depth;ctx->OMGetRenderTargets(0,nullptr,&depth);
        if(depth && !DepthViews(depth.Get()))return false;
        ID3D11RenderTargetView* rt[8]{};
        Ptr<ID3D11PixelShader> ps;Ptr<ID3D11DepthStencilState> ds;UINT stencil=0;
        ID3D11ClassInstance* instances[256]{};UINT instanceCount=256;
        Ptr<ID3D11BlendState> blend;FLOAT factors[4];UINT sampleMask=0;
        Ptr<ID3D11ShaderResourceView> previousDepth;ctx->PSGetShaderResources(0,1,&previousDepth);
        ID3D11UnorderedAccessView* uavs[64]{};UINT counts[64];
        for(auto& count:counts)count=0xffffffff;
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(8,rt,nullptr,0,uavSlots,uavs);
        ctx->PSGetShader(&ps,instances,&instanceCount);
        ctx->OMGetDepthStencilState(&ds,&stencil);
        ctx->OMGetBlendState(&blend,factors,&sampleMask);
        auto* output=target.Get();ID3D11UnorderedAccessView* emptyUavs[64]{};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(1,&output,depth?readOnlyDepth.Get():nullptr,1,uavSlots-1,emptyUavs,counts);
        // Replay must pass the same rasterized (including bias) scene depth.
        // Sample stored depth: SV_Position.z is not the depth-buffer value after bias.
        auto* input=depth?sourceDepth.Get():nullptr;ctx->PSSetShaderResources(0,1,&input);
        ctx->PSSetShader(depth?visibleDepthShader.Get():shader.Get(),nullptr,0);
        ctx->OMSetDepthStencilState(depth?equalDepth.Get():noDepth.Get(),0);
        ctx->OMSetBlendState(noBlend.Get(),nullptr,0xffffffff);
        draw();
        input=nullptr;ctx->PSSetShaderResources(0,1,&input);
        UINT rtCount=0;for(UINT i=0;i<8;++i)if(rt[i])rtCount=i+1;
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtCount,rt,depth.Get(),rtCount,uavSlots-rtCount,uavs+rtCount,counts);
        ctx->PSSetShader(ps.Get(),instances,instanceCount);
        input=previousDepth.Get();ctx->PSSetShaderResources(0,1,&input);
        ctx->OMSetDepthStencilState(ds.Get(),stencil);
        ctx->OMSetBlendState(blend.Get(),factors,sampleMask);
        for(auto* r:rt)if(r)r->Release();
        for(UINT i=0;i<uavSlots;++i)if(uavs[i])uavs[i]->Release();
        for(UINT i=0;i<instanceCount;++i)if(instances[i])instances[i]->Release();
        return true;
    }
};
