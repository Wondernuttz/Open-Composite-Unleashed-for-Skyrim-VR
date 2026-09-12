// Included by DensityMaskManager.cpp to share the complete graphics-state guard.
// Compact surviving 2x2 quads first, then write only holes into the original MRTs.
// No full texture copies, typed-UAV loads, or consumer resource substitutions.
namespace {
constexpr char kPackedResolveShader[] = R"HLSL(
cbuffer Params : register(b0) {
    float2 eyeOrigin; float2 eyeSize; float2 projectionCenter; float2 packedOrigin;
    float3 radius; float compatibilityMode; uint4 ringRates;
};
Texture2D<float4> S0:register(t0); Texture2D<float4> S1:register(t1);
Texture2D<float4> S2:register(t2); Texture2D<float4> S3:register(t3);
Texture2D<float4> S4:register(t4); Texture2D<float4> S5:register(t5);
Texture2D<float4> S6:register(t6); Texture2D<float4> S7:register(t7);
Texture2D<float> Coverage:register(t8);
struct Output {
#ifdef TARGET_0
    float4 v0:SV_Target0;
#endif
#ifdef TARGET_1
    float4 v1:SV_Target1;
#endif
#ifdef TARGET_2
    float4 v2:SV_Target2;
#endif
#ifdef TARGET_3
    float4 v3:SV_Target3;
#endif
#ifdef TARGET_4
    float4 v4:SV_Target4;
#endif
#ifdef TARGET_5
    float4 v5:SV_Target5;
#endif
#ifdef TARGET_6
    float4 v6:SV_Target6;
#endif
#ifdef TARGET_7
    float4 v7:SV_Target7;
#endif
};
Output ReadOutputs(int2 p) {
    Output o;
#ifdef TARGET_0
    o.v0=S0.Load(int3(p,0));
#endif
#ifdef TARGET_1
    o.v1=S1.Load(int3(p,0));
#endif
#ifdef TARGET_2
    o.v2=S2.Load(int3(p,0));
#endif
#ifdef TARGET_3
    o.v3=S3.Load(int3(p,0));
#endif
#ifdef TARGET_4
    o.v4=S4.Load(int3(p,0));
#endif
#ifdef TARGET_5
    o.v5=S5.Load(int3(p,0));
#endif
#ifdef TARGET_6
    o.v6=S6.Load(int3(p,0));
#endif
#ifdef TARGET_7
    o.v7=S7.Load(int3(p,0));
#endif
    return o;
}
uint2 RateSize(uint rate) {
    if(rate==1)return uint2(1,2);if(rate==2)return uint2(2,1);
    if(rate==3)return uint2(2,2);if(rate==4)return uint2(2,4);
    if(rate==5)return uint2(4,2);if(rate==6)return uint2(4,4);return uint2(1,1);
}
// x/y = quad stride; z = checkerboard layout; full-rate is (1,1,0).
uint3 Layout(uint2 cluster) {
    float distance=length(float2(cluster)*8.0/eyeSize-projectionCenter)*2.0;
    if(ringRates.w) {
        uint rate=distance<radius.x?ringRates.x:(distance<radius.y?ringRates.y:ringRates.z);
        return uint3(RateSize(rate),0);
    }
    if(distance<radius.x)return uint3(1,1,0);
    if(compatibilityMode>.5 || distance<radius.y)return uint3(2,1,1);
    return distance<radius.z?uint3(2,2,0):uint3(4,4,0);
}
Output Gather(float4 pos:SV_POSITION) {
    uint2 compact=uint2(pos.xy-packedOrigin);uint2 cluster=compact/uint2(4,8);
    if(any((cluster+1)*8>uint2(eyeSize)))discard;
    uint3 layout=Layout(cluster);
    if(layout.x==1 && layout.y==1)discard;
    // All supported layouts have a known hole in a complete eligible cluster.
    uint2 hole=uint2(layout.x>1?2:0,layout.y>1?2:0);
    if(Coverage.Load(int3(uint2(eyeOrigin)+cluster*8+hole,0))<.5)discard;
    uint lane=(compact.y%8)*4+(compact.x%4);uint quad=lane/4;
    uint2 h;
    if(layout.z) {h.y=quad/2;h.x=(quad%2)*2+(h.y&1);}
    else {uint columns=4/layout.x;h=uint2((quad%columns)*layout.x,(quad/columns)*layout.y);}
    if(any(h>=4))discard;
    uint2 pixel=uint2(eyeOrigin)+cluster*8+h*2+uint2(lane&1,(lane>>1)&1);
    return ReadOutputs(int2(pixel));
}
Output Scatter(float4 pos:SV_POSITION) {
    int2 p=int2(pos.xy);
    if(Coverage.Load(int3(p,0))<.5)discard;
    uint2 local=uint2(p-int2(eyeOrigin));uint2 cluster=local/8;
    if(any((cluster+1)*8>uint2(eyeSize)))discard;
    uint3 layout=Layout(cluster);uint2 h=(local%8)/2;
    if(layout.x==1 && layout.y==1)discard;
    uint quad;
    if(layout.z) {
        // Checkerboard reconstruction borrows the adjacent surviving quad.
        if((h.x&1)!=(h.y&1))h.x=(h.y&1)==0?h.x-1:h.x+1;
        quad=h.y*2+h.x/2;
    } else { h-=h%layout.xy;quad=(h.y/layout.y)*(4/layout.x)+h.x/layout.x; }
    uint lane=quad*4+(local.y&1)*2+(local.x&1);
    int2 packed=int2(packedOrigin)+int2(cluster*uint2(4,8))+int2(lane%4,lane/4);
    return ReadOutputs(packed);
}
)HLSL";
void NamePacked(ID3D11DeviceChild* child, const char* name)
{
    if (child) child->SetPrivateData(WKPDID_D3DDebugObjectName, UINT(std::strlen(name)), name);
}
}

void DensityMaskManager::ReleasePackedResources()
{
    for (unsigned i=0;i<8;++i) {
        ReleasePtr(originalSRVs[i]); ReleasePtr(originalRTVs[i]); originalOwners[i]=nullptr;
        ReleasePtr(packedSRVs[i]); ReleasePtr(packedRTVs[i]); ReleasePtr(packedTextures[i]);
        packedFormats[i]=DXGI_FORMAT_UNKNOWN;
    }
    packedWidth=packedHeight=packedCount=packedMask=0;
}

bool DensityMaskManager::PrepareMRTTargets(ID3D11Texture2D* const* targets, unsigned count,
    int width, int height, const EyeRegion& left, const EyeRegion& right)
{
    armed=false;
    if (!available || !targets || !count || count>8 || !ValidateGeometry(width,height,left,right))return false;
    const unsigned compactWidth=unsigned((left.width+7)/8+(right.width+7)/8)*4;
    const unsigned compactHeight=unsigned((std::max(left.height,right.height)+7)/8)*8;
    if (compactWidth!=packedWidth || compactHeight!=packedHeight) ReleasePackedResources();
    unsigned mask=0;
    for (unsigned i=0;i<8;++i) {
        auto* target=i<count?targets[i]:nullptr;
        if (!target) {
            ReleasePtr(originalSRVs[i]); ReleasePtr(originalRTVs[i]); originalOwners[i]=nullptr;
            continue;
        }
        mask|=1u<<i;
        D3D11_TEXTURE2D_DESC desc{}; target->GetDesc(&desc);
        const auto format=TypedColorFormat(desc.Format);
        if (desc.Width!=UINT(width) || desc.Height!=UINT(height) || desc.MipLevels!=1 || desc.ArraySize!=1 ||
            desc.SampleDesc.Count!=1 || !(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE) ||
            !(desc.BindFlags&D3D11_BIND_RENDER_TARGET) || format==DXGI_FORMAT_UNKNOWN)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=format;sv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;sv.Texture2D.MipLevels=1;
        D3D11_RENDER_TARGET_VIEW_DESC rt{};rt.Format=format;rt.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        if (originalOwners[i]!=target || !originalSRVs[i] || !originalRTVs[i]) {
            ReleasePtr(originalSRVs[i]);ReleasePtr(originalRTVs[i]);originalOwners[i]=nullptr;
            if (FAILED(device->CreateShaderResourceView(target,&sv,&originalSRVs[i])) ||
                FAILED(device->CreateRenderTargetView(target,&rt,&originalRTVs[i])))return false;
            originalOwners[i]=target;
            NamePacked(originalSRVs[i],"OCU RDM original MRT read");NamePacked(originalRTVs[i],"OCU RDM original MRT holes only");
        }
        if (packedFormats[i]!=format || !packedSRVs[i] || !packedRTVs[i]) {
            ReleasePtr(packedSRVs[i]);ReleasePtr(packedRTVs[i]);ReleasePtr(packedTextures[i]);packedFormats[i]=DXGI_FORMAT_UNKNOWN;
            D3D11_TEXTURE2D_DESC compact{};compact.Width=compactWidth;compact.Height=compactHeight;
            compact.ArraySize=compact.MipLevels=compact.SampleDesc.Count=1;compact.Format=format;
            compact.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
            if (FAILED(device->CreateTexture2D(&compact,nullptr,&packedTextures[i])) ||
                FAILED(device->CreateShaderResourceView(packedTextures[i],&sv,&packedSRVs[i])) ||
                FAILED(device->CreateRenderTargetView(packedTextures[i],&rt,&packedRTVs[i])))return false;
            packedFormats[i]=format;
            NamePacked(packedTextures[i],"OCU RDM packed surviving quads");
            NamePacked(packedSRVs[i],"OCU RDM packed survivors SRV");NamePacked(packedRTVs[i],"OCU RDM packed survivors RTV");
        }
    }
    if (!mask)return false;
    if (!gatherShaders[mask] || !scatterShaders[mask]) {
        ReleasePtr(gatherShaders[mask]);ReleasePtr(scatterShaders[mask]);
        std::string source;
        for(unsigned i=0;i<8;++i)if(mask&(1u<<i))source+="#define TARGET_"+std::to_string(i)+"\n";
        source+=kPackedResolveShader;
        ID3DBlob *gather=nullptr,*scatter=nullptr;
        bool ok=CompileShader(source.c_str(),"Gather","ps_5_0",&gather) &&
            CompileShader(source.c_str(),"Scatter","ps_5_0",&scatter);
        if(ok)ok=SUCCEEDED(device->CreatePixelShader(gather->GetBufferPointer(),gather->GetBufferSize(),nullptr,&gatherShaders[mask])) &&
            SUCCEEDED(device->CreatePixelShader(scatter->GetBufferPointer(),scatter->GetBufferSize(),nullptr,&scatterShaders[mask]));
        ReleasePtr(gather);ReleasePtr(scatter);
        if(!ok)return false;
        NamePacked(gatherShaders[mask],"OCU RDM gather MRT survivors PS");NamePacked(scatterShaders[mask],"OCU RDM scatter MRT holes PS");
    }
    packedWidth=compactWidth;packedHeight=compactHeight;packedCount=count;packedMask=mask;
    renderWidth=width;renderHeight=height;eyeRegions[0]=left;eyeRegions[1]=right;
    armed=true;return true;
}

bool DensityMaskManager::DrawPackedEye(int eye, bool gather, ID3D11ShaderResourceView* coverage)
{
    const auto& r=eyeRegions[eye];
    const unsigned offset=eye?unsigned((eyeRegions[0].width+7)/8)*4:0;
    ReconstructConstants constants{};
    constants.eyeOrigin[0]=float(r.left);constants.eyeOrigin[1]=float(r.top);
    constants.eyeSize[0]=float(r.width);constants.eyeSize[1]=float(r.height);
    constants.projectionCenter[0]=projX[eye];constants.projectionCenter[1]=projY[eye];
    constants.projectionPadding[0]=float(offset);
    constants.radius[0]=patternSettings.innerRadius;
    constants.radius[1]=std::max(constants.radius[0],patternSettings.midRadius);
    constants.radius[2]=std::max(constants.radius[1],1.f);
    constants.compatibilityMode=patternSettings.compatibilityMode?1.f:0.f;
    constants.ringRates[0]=unsigned(patternSettings.rates.inner);constants.ringRates[1]=unsigned(patternSettings.rates.mid);
    constants.ringRates[2]=unsigned(patternSettings.rates.outer);constants.ringRates[3]=patternSettings.customEyeRates?1u:0u;
    D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(reconstructCB,0,D3D11_MAP_WRITE_DISCARD,0,&map)))return false;
    std::memcpy(map.pData,&constants,sizeof(constants));context->Unmap(reconstructCB,0);
    context->VSSetShader(reconstructVS,nullptr,0);
    context->PSSetShader(gather?gatherShaders[packedMask]:scatterShaders[packedMask],nullptr,0);
    context->GSSetShader(nullptr,nullptr,0);context->HSSetShader(nullptr,nullptr,0);context->DSSetShader(nullptr,nullptr,0);
    context->PSSetConstantBuffers(0,1,&reconstructCB);
    context->IASetInputLayout(nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetBlendState(nullptr,nullptr,0xffffffffu);context->OMSetDepthStencilState(noDepthState,0);context->RSSetState(rasterizerState);
    ID3D11ShaderResourceView* none[9]{};context->PSSetShaderResources(0,9,none);
    ID3D11RenderTargetView* outputs[8]{};
    for(unsigned i=0;i<packedCount;++i)if(packedMask&(1u<<i))outputs[i]=gather?packedRTVs[i]:originalRTVs[i];
    context->OMSetRenderTargets(packedCount,outputs,nullptr);
    ID3D11ShaderResourceView* sources[9]{};
    for(unsigned i=0;i<packedCount;++i)if(packedMask&(1u<<i))sources[i]=gather?originalSRVs[i]:packedSRVs[i];
    sources[8]=coverage;context->PSSetShaderResources(0,9,sources);
    D3D11_VIEWPORT vp{gather?float(offset):float(r.left),gather?0.f:float(r.top),
        gather?float((r.width+7)/8*4):float(r.width),gather?float((r.height+7)/8*8):float(r.height),0,1};
    D3D11_RECT scissor{LONG(vp.TopLeftX),LONG(vp.TopLeftY),LONG(vp.TopLeftX+vp.Width),LONG(vp.TopLeftY+vp.Height)};
    context->RSSetViewports(1,&vp);context->RSSetScissorRects(1,&scissor);context->Draw(3,0);
    return true;
}

bool DensityMaskManager::ResolveMRTs(ID3D11ShaderResourceView* coverage, unsigned eyes)
{
    if(!available || !armed || !packedMask || !coverage)return false;
    PipelineState state;state.Capture(context);
    bool ok=true;
    // Finish gathering every eye before any original MRT is modified.
    for(int eye=0;eye<2 && ok;++eye)if(eyes&(1u<<eye))ok=DrawPackedEye(eye,true,coverage);
    for(int eye=0;eye<2 && ok;++eye)if(eyes&(1u<<eye))ok=DrawPackedEye(eye,false,coverage);
    state.Restore(context);return ok;
}
