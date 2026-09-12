// Same-draw ownership capture for a shader-free depth prepass. The geometry,
// skinning, rasterization and original depth/stencil tests still run once.
// Unsupported depth producers may invalidate their coverage with a depth-equal,
// read-only replay, or invalidate the entire guide when replay is not valid.
struct DensityMaskManager::GuidePass {
    bool active = false;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    UINT first = 0, count = 0;
    ID3D11RenderTargetView* rtvs[8]{};
    UINT rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil = 0;
    ID3D11BlendState* blend = nullptr;
    FLOAT factor[4]{};
    UINT sampleMask = 0;

    void Capture(ID3D11DeviceContext* ctx)
    {
        // Only save the bindings this pass changes. This runs for engine mesh
        // draws, so capturing every shader stage here would add significant CPU work.
        ctx->PSGetShader(&ps,nullptr,nullptr);
        ctx->QueryInterface(IID_PPV_ARGS(&context1));
        if(context1)context1->PSGetConstantBuffers1(0,1,&cb,&first,&count);
        else ctx->PSGetConstantBuffers(0,1,&cb);
        ctx->OMGetRenderTargets(8,rtvs,&dsv);
        rtvCount=0;for(UINT i=0;i<8;++i)if(rtvs[i])rtvCount=i+1;
        ctx->OMGetDepthStencilState(&depth,&stencil);
        ctx->OMGetBlendState(&blend,factor,&sampleMask);
        active=true;
    }
    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->OMSetRenderTargets(rtvCount,rtvs,dsv);
        ctx->OMSetDepthStencilState(depth,stencil);
        ctx->OMSetBlendState(blend,factor,sampleMask);
        if(context1)context1->PSSetConstantBuffers1(0,1,&cb,&first,&count);
        else ctx->PSSetConstantBuffers(0,1,&cb);
        ctx->PSSetShader(ps,nullptr,0);
        ReleasePtr(ps);ReleasePtr(cb);ReleasePtr(context1);
        for(auto*& rtv:rtvs)ReleasePtr(rtv);
        ReleasePtr(dsv);ReleasePtr(depth);ReleasePtr(blend);
        active=false;
    }
};

bool DensityMaskManager::PrepareDepthGuide(ID3D11Texture2D* depth)
{
    if(!available || !depth)return false;
    D3D11_TEXTURE2D_DESC desc{};depth->GetDesc(&desc);
    if(desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1)return false;
    if(!guidePS || !guideCB || !guideInvalidateDepth) {
        ReleasePtr(guidePS);ReleasePtr(guideCB);ReleasePtr(guideInvalidateDepth);
        constexpr char shader[]=R"HLSL(
cbuffer Surface:register(b0){uint draw;uint3 padding;};
uint2 main(float4 position:SV_POSITION):SV_Target0 {return uint2(draw,asuint(position.z));}
)HLSL";
        ID3DBlob* code=nullptr;
        if(!CompileShader(shader,"main","ps_5_0",&code))return false;
        HRESULT hr=device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&guidePS);
        ReleasePtr(code);if(FAILED(hr))return false;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        cb.Usage=D3D11_USAGE_DYNAMIC;cb.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if(FAILED(device->CreateBuffer(&cb,nullptr,&guideCB)))return false;
        D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthFunc=D3D11_COMPARISON_EQUAL;
        ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
        if(FAILED(device->CreateDepthStencilState(&ds,&guideInvalidateDepth)))return false;
        NamePacked(guidePS,"OCU RDM depth-draw ownership PS");NamePacked(guideCB,"OCU RDM depth-draw ID");
    }
    if(!guideSRV || !guideRTV || desc.Width!=guideWidth || desc.Height!=guideHeight) {
        ReleasePtr(guideTexture);ReleasePtr(guideRTV);ReleasePtr(guideSRV);
        desc.Format=DXGI_FORMAT_R32G32_UINT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.MiscFlags=0;
        if(FAILED(device->CreateTexture2D(&desc,nullptr,&guideTexture)) ||
            FAILED(device->CreateRenderTargetView(guideTexture,nullptr,&guideRTV)) ||
            FAILED(device->CreateShaderResourceView(guideTexture,nullptr,&guideSRV)))return false;
        guideWidth=desc.Width;guideHeight=desc.Height;
        NamePacked(guideTexture,"OCU RDM draw ownership and original raster depth");
        NamePacked(guideSRV,"OCU RDM ownership guide SRV");NamePacked(guideRTV,"OCU RDM ownership guide RTV");
    }
    ClearDepthGuide();return true;
}

void DensityMaskManager::ClearDepthGuide()
{
    guideDraws=0;
    if(guideRTV){const float zero[4]{};context->ClearRenderTargetView(guideRTV,zero);}
}

bool DensityMaskManager::BeginDepthGuide(ID3D11DepthStencilView* depth, bool invalidate)
{
    if(!guideRTV || !guidePS || !depth || (guidePass && guidePass->active) || (!invalidate && guideDraws==0xffffffffu))return false;
    const unsigned id=invalidate?0:guideDraws+1;
    D3D11_MAPPED_SUBRESOURCE map{};
    if(FAILED(context->Map(guideCB,0,D3D11_MAP_WRITE_DISCARD,0,&map)))return false;
    const unsigned values[4]{id,0,0,0};std::memcpy(map.pData,values,sizeof(values));context->Unmap(guideCB,0);
    if(!guidePass)guidePass=std::make_unique<GuidePass>();
    guidePass->Capture(context);
    context->PSSetShader(guidePS,nullptr,0);context->PSSetConstantBuffers(0,1,&guideCB);
    context->OMSetRenderTargets(1,&guideRTV,depth);context->OMSetBlendState(nullptr,nullptr,0xffffffffu);
    if(invalidate)context->OMSetDepthStencilState(guideInvalidateDepth,0);
    else ++guideDraws;
    return true;
}

void DensityMaskManager::EndDepthGuide()
{
    if(guidePass && guidePass->active)guidePass->Restore(context);
}
