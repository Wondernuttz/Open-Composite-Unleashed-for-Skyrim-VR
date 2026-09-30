#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
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
static void TestDepthGuide(ID3D11Device* d,ID3D11DeviceContext* c){
    auto guide=std::make_unique<DensityMaskManager>();Require(guide->Initialize(d),"guide manager init");
    constexpr UINT width=8,height=8;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
    desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_D32_FLOAT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ComPtr<ID3D11Texture2D> depth;HR(d->CreateTexture2D(&desc,nullptr,&depth));
    ComPtr<ID3D11DepthStencilView> dsv;HR(d->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
    desc.Format=DXGI_FORMAT_R32G32_UINT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color;HR(d->CreateTexture2D(&desc,nullptr,&color));
    ComPtr<ID3D11RenderTargetView> rtv;HR(d->CreateRenderTargetView(color.Get(),nullptr,&rtv));
    const unsigned values[128]{99,98,97,96};
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(values);buffer.Usage=D3D11_USAGE_IMMUTABLE;
    buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;D3D11_SUBRESOURCE_DATA initial{};initial.pSysMem=values;
    ComPtr<ID3D11Buffer> sentinelCB;HR(d->CreateBuffer(&buffer,&initial,&sentinelCB));
    ComPtr<ID3D11DeviceContext1> context1;c->QueryInterface(IID_PPV_ARGS(&context1));
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    const bool offsets=context1&&SUCCEEDED(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&options.ConstantBufferOffsetting;
    const UINT firstConstant=16,constantCount=16;
    constexpr char shader[]=R"HLSL(
float4 main(uint id:SV_VertexID):SV_POSITION {
    float2 uv=float2((id<<1)&2,id&2);return float4(uv*float2(2,-2)+float2(-1,1),DEPTH,1);
}
)HLSL";
    auto vertexShader=[&](const char* depthValue){
        const D3D_SHADER_MACRO defines[]={{"DEPTH",depthValue},{nullptr,nullptr}};
        ComPtr<ID3DBlob> code,errors;HR(D3DCompile(shader,sizeof(shader)-1,nullptr,defines,nullptr,"main","vs_5_0",0,0,&code,&errors));
        ComPtr<ID3D11VertexShader> result;HR(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&result));return result;
    };
    auto vs=vertexShader("0.5"),wrongDepthVS=vertexShader("0.25");
    D3D11_DEPTH_STENCIL_DESC depthDesc{};depthDesc.DepthEnable=TRUE;
    depthDesc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthDesc.DepthFunc=D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> depthState;HR(d->CreateDepthStencilState(&depthDesc,&depthState));
    D3D11_BLEND_DESC blendDesc{};blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;HR(d->CreateBlendState(&blendDesc,&blend));
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;
    rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> raster;HR(d->CreateRasterizerState(&rasterDesc,&raster));
    const FLOAT factors[4]{.125f,.25f,.5f,.75f};
    auto bind=[&]{
        c->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());c->OMSetDepthStencilState(depthState.Get(),17);
        c->OMSetBlendState(blend.Get(),factors,0xffffffffu);
        c->PSSetShader(nullptr,nullptr,0);c->PSSetConstantBuffers(0,1,sentinelCB.GetAddressOf());
        if(offsets)context1->PSSetConstantBuffers1(0,1,sentinelCB.GetAddressOf(),&firstConstant,&constantCount);
    };
    auto restored=[&]{
        ComPtr<ID3D11PixelShader> ps;c->PSGetShader(&ps,nullptr,nullptr);Require(!ps,"guide PS restoration");
        ComPtr<ID3D11Buffer> cb;c->PSGetConstantBuffers(0,1,&cb);Require(cb==sentinelCB,"guide CB restoration");
        if(offsets){ComPtr<ID3D11Buffer> ranged;UINT first=0,count=0;context1->PSGetConstantBuffers1(0,1,&ranged,&first,&count);
            Require(ranged==sentinelCB&&first==firstConstant&&count==constantCount,"guide D3D11.1 constant range restoration");}
        ComPtr<ID3D11RenderTargetView> rt;ComPtr<ID3D11DepthStencilView> dv;c->OMGetRenderTargets(1,&rt,&dv);
        Require(rt==rtv&&dv==dsv,"guide target restoration");
        ComPtr<ID3D11DepthStencilState> ds;UINT stencil=0;c->OMGetDepthStencilState(&ds,&stencil);
        Require(ds==depthState&&stencil==17,"guide depth-state restoration");
        ComPtr<ID3D11BlendState> bs;FLOAT f[4]{};UINT mask=0;c->OMGetBlendState(&bs,f,&mask);
        Require(bs==blend&&mask==0xffffffffu&&!std::memcmp(f,factors,sizeof(f)),"guide blend restoration");
    };
    auto owners=[&](unsigned expected){
        ComPtr<ID3D11Resource> resource;guide->DepthGuide()->GetResource(&resource);
        ComPtr<ID3D11Texture2D> texture;HR(resource.As(&texture));
        const auto pixels=Read(d,c,texture.Get(),8);const float z=.5f;unsigned depthBits=0;std::memcpy(&depthBits,&z,4);
        for(size_t i=0;i<pixels.size();i+=8){unsigned sample[2]{};std::memcpy(sample,pixels.data()+i,8);
            Require(sample[0]==expected&&sample[1]==depthBits,"guide ownership/depth pixels");}
    };
    c->VSSetShader(vs.Get(),nullptr,0);c->RSSetState(raster.Get());
    const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};c->RSSetViewports(1,&viewport);
    c->IASetInputLayout(nullptr);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for(unsigned cycle=0;cycle<2;++cycle){
        Require(guide->PrepareDepthGuide(depth.Get()),"prepare ownership guide");bind();
        c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,1,0);
        ComPtr<ID3D11Buffer> immutable;
        auto draw=[&](bool invalidate){
            Require(guide->BeginDepthGuide(dsv.Get(),invalidate),"begin ownership guide");
            ComPtr<ID3D11Buffer> cb;c->PSGetConstantBuffers(0,1,&cb);D3D11_BUFFER_DESC bound{};cb->GetDesc(&bound);
            Require(bound.ByteWidth==16&&bound.BindFlags==D3D11_BIND_CONSTANT_BUFFER,"guide constant contract");
            if(invalidate){
                Require(bound.Usage==D3D11_USAGE_IMMUTABLE&&bound.CPUAccessFlags==0,"invalidation must use immutable constants");
                if(immutable)Require(cb==immutable,"invalidation constant resource reused");else immutable=cb;
                ComPtr<ID3D11DepthStencilState> ds;c->OMGetDepthStencilState(&ds,nullptr);D3D11_DEPTH_STENCIL_DESC state{};ds->GetDesc(&state);
                Require(state.DepthEnable&&state.DepthFunc==D3D11_COMPARISON_EQUAL&&state.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO,"invalidation depth-equal/read-only contract");
            }else Require(bound.Usage==D3D11_USAGE_DYNAMIC&&bound.CPUAccessFlags==D3D11_CPU_ACCESS_WRITE,"positive ownership keeps dynamic IDs");
            c->Draw(3,0);guide->EndDepthGuide();restored();
        };
        draw(false);owners(1);draw(false);owners(2);
        const auto originalDepth=Read(d,c,depth.Get(),4);
        for(unsigned repeat=0;repeat<8;++repeat){draw(true);owners(0);}
        Require(Read(d,c,depth.Get(),4)==originalDepth,"invalidation changed scene depth");
        draw(false);owners(3);
        c->VSSetShader(wrongDepthVS.Get(),nullptr,0);draw(true);owners(3);
        Require(Read(d,c,depth.Get(),4)==originalDepth,"rejected invalidation changed depth");
        c->VSSetShader(vs.Get(),nullptr,0);draw(true);owners(0);
        guide->Shutdown();Require(!guide->DepthGuide(),"shutdown released ownership guide");
        // A fresh scope owns a fresh manager; failed-session retry stays latched.
        if(cycle==0){guide=std::make_unique<DensityMaskManager>();Require(guide->Initialize(d),"guide session recreation");}
    }
    c->ClearState();
    std::puts("DEPTH GUIDE PASS: immutable invalidation constants, advancing IDs, repeated zero ownership, depth equality, read-only depth, state restoration, session recreation");
}
static void TestResolveLifecycle(ID3D11Device* d,ID3D11DeviceContext* c){
    // Keep both managers alive across same-size resource replacement and resize.
    // The legacy reference has no sparse-resolve cache and uses a separate copy.
    DensityMaskManager legacy,packed;
    Require(legacy.Initialize(d)&&packed.Initialize(d),"lifecycle managers init");
    // Preserve both game-stage buffers and ranges through reconstruction.
    // Distinct resources and nonzero ranges expose incomplete state restoration.
    const unsigned sentinelValues[128]{17,31,59,113};
    D3D11_BUFFER_DESC sentinelDesc{};sentinelDesc.ByteWidth=sizeof(sentinelValues);
    sentinelDesc.Usage=D3D11_USAGE_IMMUTABLE;sentinelDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sentinelData{};sentinelData.pSysMem=sentinelValues;
    ComPtr<ID3D11Buffer> sentinelVS,sentinelPS;
    HR(d->CreateBuffer(&sentinelDesc,&sentinelData,&sentinelVS));
    HR(d->CreateBuffer(&sentinelDesc,&sentinelData,&sentinelPS));
    ComPtr<ID3D11DeviceContext1> context1;c->QueryInterface(IID_PPV_ARGS(&context1));
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    const bool offsets=context1&&SUCCEEDED(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS,&options,sizeof(options)))&&options.ConstantBufferOffsetting;
    const UINT firstConstant=16,constantCount=16;
    auto bindConstants=[&]{
        if(offsets){
            context1->VSSetConstantBuffers1(0,1,sentinelVS.GetAddressOf(),&firstConstant,&constantCount);
            context1->PSSetConstantBuffers1(0,1,sentinelPS.GetAddressOf(),&firstConstant,&constantCount);
        }else{
            c->VSSetConstantBuffers(0,1,sentinelVS.GetAddressOf());
            c->PSSetConstantBuffers(0,1,sentinelPS.GetAddressOf());
        }
    };
    auto requireConstants=[&]{
        ComPtr<ID3D11Buffer> vs,ps;
        c->VSGetConstantBuffers(0,1,&vs);c->PSGetConstantBuffers(0,1,&ps);
        Require(vs==sentinelVS&&ps==sentinelPS,"packed VS/PS b0 resource restoration");
        if(offsets){
            ComPtr<ID3D11Buffer> rangedVS,rangedPS;UINT vsFirst=0,vsCount=0,psFirst=0,psCount=0;
            context1->VSGetConstantBuffers1(0,1,&rangedVS,&vsFirst,&vsCount);
            context1->PSGetConstantBuffers1(0,1,&rangedPS,&psFirst,&psCount);
            Require(rangedVS==sentinelVS&&rangedPS==sentinelPS&&
                vsFirst==firstConstant&&psFirst==firstConstant&&
                vsCount==constantCount&&psCount==constantCount,"packed VS/PS b0 range restoration");
        }
    };
    unsigned comparisons=0;
    for(unsigned geometry=0;geometry<4;++geometry){
        const UINT width=geometry==2?134:250,height=geometry==2?70:73;
        const DensityMaskManager::EyeRegion eyes[]={
            geometry==2?DensityMaskManager::EyeRegion{0,0,67,70}:DensityMaskManager::EyeRegion{3,2,107,67},
            geometry==2?DensityMaskManager::EyeRegion{67,0,67,70}:DensityMaskManager::EyeRegion{117,1,129,70}};
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;
        desc.ArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        std::array<ComPtr<ID3D11Texture2D>,8> colors;
        ID3D11Texture2D* targets[8]{};
        const UINT8 channels[8]={15,0,5,0,0,0,0,8};
        // Slot 5 is bound but has no writable channels; 1/3/4/6 are MRT gaps.
        for(unsigned slot:{0u,2u,5u,7u}){
            HR(d->CreateTexture2D(&desc,nullptr,&colors[slot]));targets[slot]=colors[slot].Get();
        }
        ComPtr<ID3D11Texture2D> reference,coverage,externalCoverage,depth,eligibility;
        HR(d->CreateTexture2D(&desc,nullptr,&reference));
        desc.Format=DXGI_FORMAT_R8_UNORM;
        HR(d->CreateTexture2D(&desc,nullptr,&coverage));
        HR(d->CreateTexture2D(&desc,nullptr,&externalCoverage));
        ComPtr<ID3D11RenderTargetView> coverageRT;
        ComPtr<ID3D11ShaderResourceView> coverageSRV,externalSRV,eligibilitySRV;
        HR(d->CreateRenderTargetView(coverage.Get(),nullptr,&coverageRT));
        HR(d->CreateShaderResourceView(coverage.Get(),nullptr,&coverageSRV));
        HR(d->CreateShaderResourceView(externalCoverage.Get(),nullptr,&externalSRV));
        desc.Format=DXGI_FORMAT_D32_FLOAT;desc.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        HR(d->CreateTexture2D(&desc,nullptr,&depth));
        ComPtr<ID3D11DepthStencilView> dsv;HR(d->CreateDepthStencilView(depth.Get(),nullptr,&dsv));
        const UINT clusterWidth=(eyes[0].width+7)/8+(eyes[1].width+7)/8;
        const UINT clusterHeight=(std::max(eyes[0].height,eyes[1].height)+7)/8;
        desc.Width=clusterWidth;desc.Height=clusterHeight;
        desc.Format=DXGI_FORMAT_R32_FLOAT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        HR(d->CreateTexture2D(&desc,nullptr,&eligibility));
        HR(d->CreateShaderResourceView(eligibility.Get(),nullptr,&eligibilitySRV));
        for(unsigned profile=0;profile<3;++profile){
            DensityMaskManager::PatternSettings pattern;
            pattern.innerRadius=.20f;pattern.midRadius=.45f;
            pattern.compatibilityMode=profile==0;pattern.customEyeRates=profile!=0;
            // Include non-monotonic rates: an optimized bound cannot assume
            // that only the outer region needs reconstruction.
            pattern.rates=profile==1?ocu_foveation::RingRates{ocu_foveation::Rate::X4x4,
                ocu_foveation::Rate::X2x2,ocu_foveation::Rate::X1x1}:
                ocu_foveation::RingRates{ocu_foveation::Rate::X1x1,
                    ocu_foveation::Rate::X2x1,ocu_foveation::Rate::X4x2};
            pattern.horizontalScale=profile==0?.5f:(profile==1?2.f:1.f);
            const float lx=profile==0?.12f:(profile==1?.8f:.5f),ly=profile==0?.83f:.4f;
            const float rx=profile==0?.89f:(profile==1?.2f:.5f),ry=profile==0?.19f:.6f;
            legacy.SetProjectionCenters(lx,ly,rx,ry);packed.SetProjectionCenters(lx,ly,rx,ry);
            legacy.SetPatternSettings(pattern);packed.SetPatternSettings(pattern);
            Require(legacy.PrepareStereoTarget(reference.Get(),width,height,eyes[0],eyes[1]),"lifecycle reference prepare");
            Require(packed.PrepareMRTTargets(targets,8,width,height,eyes[0],eyes[1],channels),"lifecycle packed prepare");
            for(unsigned mode=0;mode<6;++mode){
                // Same resource and SRV throughout full -> empty -> sparse ->
                // intentional omission -> full transitions. Production owns
                // these updates through ApplyDepthMask, even after BeginFrame.
                pattern.blackout.outer=mode==3;
                pattern.blackout.middle=mode==4;
                pattern.blackout.guardPixels=0;
                legacy.SetPatternSettings(pattern);packed.SetPatternSettings(pattern);
                if(mode==2||mode==5){
                    packed.BeginFrame();
                    Require(packed.PrepareMRTTargets(targets,8,width,height,eyes[0],eyes[1],channels),"lifecycle next-frame prepare");
                }
                std::vector<float> eligible(size_t(clusterWidth)*clusterHeight,1.f);
                for(UINT y=0;y<clusterHeight;++y)for(UINT x=0;x<clusterWidth;++x){
                    if(mode==1||(mode==2&&(x+3*y)%5!=0))eligible[size_t(y)*clusterWidth+x]=0;
                }
                c->UpdateSubresource(eligibility.Get(),0,nullptr,eligible.data(),clusterWidth*sizeof(float),0);
                const FLOAT zero[4]{};c->ClearRenderTargetView(coverageRT.Get(),zero);
                const float clearDepth=(geometry&1)?0.f:1.f,holeDepth=1.f-clearDepth;
                // Seed genuine false positives before ApplyDepthMask: native
                // pixels and intentional omissions can share the hole's depth.
                // Exact coverage checks must still preserve those pixels.
                c->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,
                    (mode==1||mode==3)?holeDepth:clearDepth,0);
                Require(packed.ApplyDepthMask(dsv.Get(),clearDepth,coverageRT.Get(),eligibilitySRV.Get()),"lifecycle owned mask");
                const auto owned=Read(d,c,coverage.Get(),1);
                const auto ownedDepth=Read(d,c,depth.Get(),sizeof(float));
                bool anyHole=false,anyOmission=false;
                for(auto value:owned){anyHole=anyHole||value>=128;anyOmission=anyOmission||value==64;}
                if(mode==1)Require(!anyHole&&!anyOmission,"empty eligibility produced coverage");
                if(mode==0||mode==2||mode==5)Require(anyHole,"lifecycle fixture has no reconstruction holes");
                if(mode==3)Require(anyOmission,"lifecycle fixture has no intentional omissions");
                // Repeated resolves switch between both eyes, left and right
                // while source colors change. A second coverage SRV is then
                // rewritten directly without an ApplyDepthMask notification.
                for(unsigned repeat=0;repeat<7;++repeat){
                    const bool external=repeat>=4;
                    auto holes=owned;
                    if(external){
                        if(repeat==5)std::fill(holes.begin(),holes.end(),0);
                        if(repeat==6)for(const auto& eye:eyes)
                            for(int y=0;y<eye.height;++y)for(int x=0;x<eye.width;++x)
                                if(((x/8)+(y/8))%3!=0)holes[size_t(eye.top+y)*width+eye.left+x]=0;
                        c->UpdateSubresource(externalCoverage.Get(),0,nullptr,holes.data(),width,0);
                    }
                    auto* mask=external?externalSRV.Get():coverageSRV.Get();
                    const unsigned eyeBits=repeat==1?1u:((repeat==2||repeat==6)?2u:3u);
                    std::array<std::vector<unsigned char>,8> expected;
                    for(unsigned slot:{0u,2u,5u,7u}){
                        std::vector<unsigned char> original(size_t(width)*height*4);
                        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
                            for(unsigned channel=0;channel<4;++channel)
                                original[(size_t(y)*width+x)*4+channel]=static_cast<unsigned char>(
                                    (x*13+y*29+channel*61+slot*19+repeat*47+mode*31+geometry*7)%256);
                        c->UpdateSubresource(colors[slot].Get(),0,nullptr,original.data(),width*4,0);
                        expected[slot]=original;
                        if(channels[slot]){
                            c->UpdateSubresource(reference.Get(),0,nullptr,original.data(),width*4,0);
                            Require(legacy.ResolveColor(reference.Get(),mask,eyeBits),"lifecycle reference resolve");
                            const auto reconstructed=Read(d,c,reference.Get(),4);
                            for(size_t i=0;i<original.size();++i)
                                if(channels[slot]&(1u<<(i%4)))expected[slot][i]=reconstructed[i];
                        }
                    }
                    bindConstants();
                    Require(packed.ResolveMRTs(mask,eyeBits),"lifecycle packed resolve");
                    requireConstants();
                    Require(Read(d,c,depth.Get(),sizeof(float))==ownedDepth,"resolve modified private scene depth");
                    for(unsigned slot:{0u,2u,5u,7u}){
                        const auto actual=Read(d,c,colors[slot].Get(),4);
                        if(actual!=expected[slot])std::printf("lifecycle geometry=%u profile=%u mode=%u repeat=%u slot=%u\n",
                            geometry,profile,mode,repeat,slot);
                        Require(actual==expected[slot],"lifecycle packed output differs from uncached reference");
                        ++comparisons;
                    }
                }
            }
        }
        c->ClearState();
    }
    std::printf("PACKED LIFECYCLE PASS: %u exact MRT comparisons; VS/PS b0 resources and ranges; owned and external same-SRV updates, sparse/empty/omission coverage, normal/reversed depth, stereo/left/right eye masks, channels, gaze/rates, frame reset and resource resize/replacement\n",comparisons);
}
static void Run(IDXGIAdapter* adapter,D3D_DRIVER_TYPE driver,bool exactBoundary=false){
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level;
    HR(D3D11CreateDevice(adapter,driver,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&d,&level,&c));
    ComPtr<ID3D11InfoQueue> debug;HR(d.As(&debug));
    TestDepthGuide(d.Get(),c.Get());
    if(!exactBoundary)TestResolveLifecycle(d.Get(),c.Get());
    const UINT width=exactBoundary?134:250,height=exactBoundary?70:73;
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
    const DensityMaskManager::EyeRegion eyes[]={exactBoundary?DensityMaskManager::EyeRegion{0,0,67,70}:DensityMaskManager::EyeRegion{3,2,107,67},
        exactBoundary?DensityMaskManager::EyeRegion{67,0,67,70}:DensityMaskManager::EyeRegion{117,1,129,70}};
    const float inner=exactBoundary?.4f:.23f,middle=exactBoundary?.7f:.57f;
    std::vector<DensityMaskManager::PatternSettings> patterns;
    patterns.push_back({inner,middle,true,false,{}});patterns.push_back({inner,middle,false,false,{}});
    for(unsigned rate=0;rate<7;rate++)patterns.push_back({inner,middle,false,true,
        {ocu_foveation::Rate(rate),ocu_foveation::Rate((rate+2)%7),ocu_foveation::Rate((rate+4)%7)}});
    const auto originalPatterns=patterns;
    for(float aspect:{.5f,1.3f,2.f})for(auto pattern:originalPatterns) {
        pattern.horizontalScale=aspect;patterns.push_back(pattern);
    }
    for(unsigned patternIndex=0;patternIndex<patterns.size();patternIndex++)for(unsigned slots:{0xffu,0x89u,0x02u}) {
        const auto& pattern=patterns[patternIndex];legacy.SetPatternSettings(pattern);packed.SetPatternSettings(pattern);
        if(exactBoundary) {legacy.SetProjectionCenters(.45f,.55f,.6f,.4f);packed.SetProjectionCenters(.45f,.55f,.6f,.4f);}
        else {legacy.SetProjectionCenters(.12f,.83f,.89f,.19f);packed.SetProjectionCenters(.12f,.83f,.89f,.19f);}
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
    std::puts("PACKED MRT PASS: all rates, checker/quarter/sixteenth, horizontal scale .5/1/1.3/2, MRT gaps, asymmetric eyes, gutters, partial tiles, PS t0-t8 restoration");
}
int main(){try{
    Run(nullptr,D3D_DRIVER_TYPE_WARP);
    Run(nullptr,D3D_DRIVER_TYPE_WARP,true);
    ComPtr<IDXGIFactory1> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));unsigned hardwareCount=0;
    for(UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> adapter;HRESULT result=factory->EnumAdapters1(index,&adapter);
        if(result==DXGI_ERROR_NOT_FOUND)break;HR(result);DXGI_ADAPTER_DESC1 desc{};HR(adapter->GetDesc1(&desc));
        if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
        std::printf("Packed RDM hardware: %ls\n",desc.Description);
        Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN);Run(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,true);++hardwareCount;
    }
    Require(hardwareCount>0,"no hardware adapters tested");return 0;
}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}}
