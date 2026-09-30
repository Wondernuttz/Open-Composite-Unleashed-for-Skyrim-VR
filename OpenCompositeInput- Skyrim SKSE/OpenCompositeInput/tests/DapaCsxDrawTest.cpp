#include "../src/DapaCsxDraw.h"
#include "../src/DapaAcceptedDraw.h"
#include "../../../OpenCompositeSkyrimVR/DrvOpenXR/DapaPlayerMaskGpu.h"
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;
using DapaAcceptedDraw::Arguments;
using DapaAcceptedDraw::Scope;
void Check(bool ok,const char* what) {if(!ok)throw std::runtime_error(what);}
void HR(HRESULT hr) {Check(SUCCEEDED(hr),"D3D11 operation failed");}

void ScopeTest() {
    Arguments args{reinterpret_cast<void*>(1),3,2,7,-9,17};
    Check(!Scope::Claim(args),"unscoped acceptance inherited player");
    Scope outer(args,true);
    std::thread other([&]{Check(!Scope::Claim(args),"ownership crossed thread");});other.join();
    auto wrong=args;wrong.context=reinterpret_cast<void*>(2);
    Check(!Scope::Claim(wrong) && !outer.consumed,"wrong context consumed scope");
    wrong=args;++wrong.first;
    Check(!Scope::Claim(wrong) && !outer.consumed,"different geometry consumed scope");
    {Scope nested(args,false);Check(!Scope::Claim(args),"unowned nested draw masked");}
    Check(!outer.consumed,"nested draw consumed outer ownership");
    unsigned real=0,mask=0;
    DapaAcceptedDraw::Dispatch(args,[&]{++real;Check(!Scope::Claim(args),"reentry duplicated mask");},[&]{++mask;});
    Check(real==1 && mask==1 && !Scope::Claim(args),"accepted draw not exactly once");
    std::puts("PASS ownership: nested, unowned, thread-local, context/arguments, reentry, exactly-once");
}

struct FakeContext {void** table;};
unsigned native=0,masked=0;
void Native(FakeContext*,UINT n,UINT count,UINT start,INT base,UINT first) {
    Check(n==123 && count==2 && start==7 && base==-9 && first==17,"accepted ABI arguments");++native;
}
void Accepted(FakeContext* ctx,UINT n,UINT count,UINT start,INT base,UINT first) {
    DapaAcceptedDraw::Dispatch({ctx,n,count,start,base,first},
        [&]{reinterpret_cast<decltype(&Native)>(ctx->table[20])(ctx,n,count,start,base,first);},[&]{++masked;});
}
void InstructionTest(const DapaEngineDraw::Site& site) {
    auto* mem=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    Check(mem!=nullptr,"fixture allocation");
    std::vector<uint8_t> bytes{0x48,0x83,0xec,0x38,
        0xba,123,0,0,0,0x41,0xb8,2,0,0,0,0x41,0xb9,7,0,0,0,
        0xc7,0x44,0x24,0x20,0xf7,0xff,0xff,0xff,0xc7,0x44,0x24,0x28,17,0,0,0,
        0x48,0x8b,0x01,0x49,0x89,0xc2};
    const auto offset=bytes.size();
    bytes.insert(bytes.end(),site.expected.begin(),site.expected.begin()+site.size);
    bytes.insert(bytes.end(),{0x48,0x83,0xc4,0x38,0xc3});
    memcpy(mem,bytes.data(),bytes.size());
    DapaEngineDraw::Hook hook;
    Check(hook.Prepare(site,mem+offset) && hook.Build(mem+512,reinterpret_cast<uintptr_t>(&Accepted)),"accepted instruction prepare");
    Check(hook.Install(),"accepted instruction install");
    void* table[21]{};table[20]=reinterpret_cast<void*>(&Native);FakeContext ctx{table};
    auto call=reinterpret_cast<void(*)(FakeContext*)>(mem);
    native=masked=0;
    {Scope scope({&ctx,123,2,7,-9,17},true);call(&ctx);}
    Check(native==1 && masked==1,"accepted fixture mask missing");
    {Scope scope({&ctx,123,2,7,-9,17},false);call(&ctx);}
    Check(native==2 && masked==1,"non-player accepted fixture masked");
    Check(hook.Remove(),"accepted instruction rollback");call(&ctx);
    Check(native==3 && masked==1,"accepted rollback changed draw");
    VirtualFree(mem,0,MEM_RELEASE);
    std::printf("PASS accepted instruction RVA %X: native arguments, player/non-player, rollback\n",site.rva);
}

const DapaCsxDraw::Build* AdapterTest(const wchar_t* path) {
    // Map the actual installed binary without entry-point execution or imports.
    HMODULE module=LoadLibraryExW(path,nullptr,DONT_RESOLVE_DLL_REFERENCES);
    Check(module!=nullptr,"map installed CSX");
    auto base=reinterpret_cast<uintptr_t>(module);
    const DapaCsxDraw::Build* build=nullptr;
    for(const auto& candidate:DapaCsxDraw::builds)
        if(DapaCsxDraw::FindBuild(base,reinterpret_cast<void*>(base+candidate.ownerRva))==&candidate) {
            Check(!build,"binary matched multiple contracts");build=&candidate;
        }
    Check(build!=nullptr,"installed CSX has no validated owner contract");
    auto* owner=reinterpret_cast<void*>(base+build->ownerRva);
    std::printf("Testing %s\n",build->name);
    auto relocatedTemplate=*build;
    relocatedTemplate.ownerRva+=0x100;
    for(auto& site:relocatedTemplate.sites)site.rva+=0x100;
    DapaCsxDraw::Build rebased{};
    Check(DapaCsxDraw::RebaseKnownOwner(relocatedTemplate,base,owner,rebased),"known code rejected solely for relocated template address");
    Check(rebased.ownerRva==build->ownerRva && rebased.sites[0].rva==build->sites[0].rva && rebased.sites[1].rva==build->sites[1].rva,"relocated draw offsets incorrect");
    Check(!DapaCsxDraw::RebaseKnownOwner(*build,base+1,owner,rebased),"wrong module accepted");
    Check(!DapaCsxDraw::RebaseKnownOwner(*build,base,static_cast<uint8_t*>(owner)+1,rebased),"interior instruction accepted as owner");
    auto corrupted=*build;corrupted.ownerSha256[0]^=1;
    Check(!DapaCsxDraw::RebaseKnownOwner(corrupted,base,owner,rebased),"unverified code accepted by rebasing");
    std::puts("PASS code discovery: full function at actual owner, rebased sites, module/boundary/hash refusal");
    DapaCsxDraw::Adapter adapter;
    Check(adapter.Prepare(base,owner,reinterpret_cast<uintptr_t>(&Accepted)),"actual CSX adapter prepare");
    Check(adapter.Install(),"actual CSX observer install");
    Check(adapter.build==build,"adapter selected wrong binary contract");
    Check(!DapaCsxDraw::MatchesOwner(*build,owner),"patched code unexpectedly matches original");
    Check(adapter.Remove() && DapaCsxDraw::MatchesOwner(*build,owner),"actual CSX rollback failed");
    DapaCsxDraw::Adapter wrong;
    Check(!wrong.Prepare(base,reinterpret_cast<void*>(base+build->ownerRva+1),reinterpret_cast<uintptr_t>(&Accepted)),"unknown owner accepted");
    std::vector<uint8_t> modified(build->ownerSize);memcpy(modified.data(),owner,modified.size());modified[100]^=1;
    Check(!DapaCsxDraw::MatchesOwner(*build,modified.data()),"changed owner function accepted");
    for(const auto& other:DapaCsxDraw::builds)if(&other!=build)
        Check(!DapaCsxDraw::MatchesOwner(other,owner),"wrong build contract accepted");
    // Every byte matters, including branch predicates and each draw site.
    for(size_t i=0;i<modified.size();++i) {
        memcpy(modified.data(),owner,modified.size());modified[i]^=1;
        Check(!DapaCsxDraw::MatchesOwner(*build,modified.data()),"owner mutation accepted");
    }
    FreeLibrary(module);
    std::puts("PASS actual installed CSX: full function SHA256, both patch sites, complete byte restoration, mismatch refusal");
    return build;
}

void GpuTest(D3D_DRIVER_TYPE driver) {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    HR(D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx));
    DapaPlayerMaskGpu mask;Check(mask.Initialize(device.Get()) && mask.Size(device.Get(),16,8),"mask resources");
    const char* shader="float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),.4,1);}";
    ComPtr<ID3DBlob> code;HR(D3DCompile(shader,strlen(shader),"accepted-draw-test",nullptr,nullptr,"main","vs_5_0",0,0,&code,nullptr));
    ComPtr<ID3D11VertexShader> vs;HR(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));
    const char* pixel="float main():SV_Target{return .75;}";
    HR(D3DCompile(pixel,strlen(pixel),"scene-test",nullptr,nullptr,"main","ps_5_0",0,0,&code,nullptr));
    ComPtr<ID3D11PixelShader> ps;HR(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&ps));
    const UINT indices[]={0,1,2};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(indices);bd.BindFlags=D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{indices,0,0};ComPtr<ID3D11Buffer> ib;HR(device->CreateBuffer(&bd,&init,&ib));
    D3D11_TEXTURE2D_DESC td{};td.Width=16;td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> scene,read;ComPtr<ID3D11RenderTargetView> rtv;
    HR(device->CreateTexture2D(&td,nullptr,&scene));HR(device->CreateRenderTargetView(scene.Get(),nullptr,&rtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&td,nullptr,&read));
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
    ComPtr<ID3D11RasterizerState> rs;HR(device->CreateRasterizerState(&rd,&rs));ctx->RSSetState(rs.Get());
    ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->IASetIndexBuffer(ib.Get(),DXGI_FORMAT_R32_UINT,0);
    auto* target=rtv.Get();ctx->OMSetRenderTargets(1,&target,nullptr);
    auto verify=[&](ID3D11Texture2D* texture,int eye,bool active,float value,float clear) {
        ctx->CopyResource(read.Get(),texture);D3D11_MAPPED_SUBRESOURCE mapped{};HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        for(int y=0;y<8;++y)for(int x=0;x<16;++x) {
            float actual;memcpy(&actual,static_cast<char*>(mapped.pData)+y*mapped.RowPitch+x*4,4);
            const float expected=active && x/8==eye?value:clear;
            Check(std::abs(actual-expected)<.00001f,"GPU pixels/eye coverage incorrect");
        }
        ctx->Unmap(read.Get(),0);
    };
    // Modes: accepted fast/slow, suppressed, redirected elsewhere, accepted non-player.
    for(int eye=0;eye<2;++eye)for(int mode=0;mode<5;++mode) {
        mask.Clear(ctx.Get());const float clear[4]={.1f,.1f,.1f,.1f};ctx->ClearRenderTargetView(target,clear);
        D3D11_VIEWPORT viewport{float(eye*8),0,8,8,0,1};ctx->RSSetViewports(1,&viewport);
        Arguments args{ctx.Get(),3,1,0,0,0};Scope scope(args,mode!=4);
        unsigned draws=0,masks=0;
        // Suppression and redirection do not reach CSX's accepted-world call sites.
        if(mode!=2 && mode!=3)DapaAcceptedDraw::Dispatch(args,[&]{++draws;ctx->DrawIndexedInstanced(3,1,0,0,0);},[&]{
            ++masks;mask.Replay(ctx.Get(),[&]{ctx->DrawIndexedInstanced(3,1,0,0,0);});
        });
        Check(draws==unsigned(mode!=2 && mode!=3) && masks==unsigned(mode<2),"dispatch counts");
        ComPtr<ID3D11RenderTargetView> got;ctx->OMGetRenderTargets(1,&got,nullptr);Check(got.Get()==target,"scene target not restored");
        ComPtr<ID3D11PixelShader> gotPS;ctx->PSGetShader(&gotPS,nullptr,nullptr);Check(gotPS.Get()==ps.Get(),"scene shader not restored");
        verify(mask.Texture(),eye,mode<2,.4f,-1.f);
        verify(scene.Get(),eye,mode!=2 && mode!=3,.75f,.1f);
    }
    std::printf("PASS %s GPU: both eyes, accepted/suppressed/redirected/non-player mask pixels, scene colour/state preserved\n",driver==D3D_DRIVER_TYPE_HARDWARE?"hardware":"WARP");
}
void BiasedDepthTest(D3D_DRIVER_TYPE driver,bool reversed,bool d24) {
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> ctx;
    HR(D3D11CreateDevice(nullptr,driver,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,nullptr,&ctx));
    DapaPlayerMaskGpu mask;Check(mask.Initialize(d.Get()) && mask.Size(d.Get(),16,8),"biased mask resources");
    mask.Clear(ctx.Get());
    auto makeVS=[&](float z) {
        char source[300];sprintf_s(source,"float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),%.8f,1);}",z);
        ComPtr<ID3DBlob> code;HR(D3DCompile(source,strlen(source),"depth-bias-test",nullptr,nullptr,"main","vs_5_0",0,0,&code,nullptr));
        ComPtr<ID3D11VertexShader> vs;HR(d->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vs));return vs;
    };
    auto front=makeVS(reversed?.6f:.4f),rear=makeVS(reversed?.4f:.6f);
    D3D11_TEXTURE2D_DESC td{};td.Width=16;td.Height=8;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=d24?DXGI_FORMAT_R24G8_TYPELESS:DXGI_FORMAT_R32_TYPELESS;td.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth,readDepth,readMask;HR(d->CreateTexture2D(&td,nullptr,&depth));
    D3D11_DEPTH_STENCIL_VIEW_DESC vd{};vd.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;vd.Format=d24?DXGI_FORMAT_D24_UNORM_S8_UINT:DXGI_FORMAT_D32_FLOAT;
    ComPtr<ID3D11DepthStencilView> dsv;HR(d->CreateDepthStencilView(depth.Get(),&vd,&dsv));
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    HR(d->CreateTexture2D(&td,nullptr,&readDepth));td.Format=DXGI_FORMAT_R32_FLOAT;HR(d->CreateTexture2D(&td,nullptr,&readMask));
    ctx->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH|(d24?D3D11_CLEAR_STENCIL:0),reversed?0.f:1.f,37);
    ctx->OMSetRenderTargets(0,nullptr,dsv.Get());
    D3D11_DEPTH_STENCIL_DESC ds{};ds.DepthEnable=TRUE;ds.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;ds.DepthFunc=reversed?D3D11_COMPARISON_GREATER_EQUAL:D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> state;HR(d->CreateDepthStencilState(&ds,&state));ctx->OMSetDepthStencilState(state.Get(),37);
    D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;rd.DepthBias=reversed?-256:256;
    ComPtr<ID3D11RasterizerState> rs;HR(d->CreateRasterizerState(&rd,&rs));ctx->RSSetState(rs.Get());
    D3D11_VIEWPORT viewport{0,0,16,8,0,1};ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(front.Get(),nullptr,0);ctx->Draw(3,0);
    ctx->CopyResource(readDepth.Get(),depth.Get());
    auto verify=[&] {
        ctx->CopyResource(readMask.Get(),mask.Texture());
        D3D11_MAPPED_SUBRESOURCE m{},z{};HR(ctx->Map(readMask.Get(),0,D3D11_MAP_READ,0,&m));HR(ctx->Map(readDepth.Get(),0,D3D11_MAP_READ,0,&z));
        for(int y=0;y<8;++y)for(int x=0;x<16;++x) {
            float actual,expected;memcpy(&actual,static_cast<char*>(m.pData)+y*m.RowPitch+x*4,4);
            if(d24) {uint32_t raw;memcpy(&raw,static_cast<char*>(z.pData)+y*z.RowPitch+x*4,4);expected=float(raw&0xffffff)/float(0xffffff);}
            else memcpy(&expected,static_cast<char*>(z.pData)+y*z.RowPitch+x*4,4);
            Check(std::abs(actual-expected)<.0000002f,"mask must match actual biased depth, not unbiassed SV_Position.z");
        }
        ctx->Unmap(readMask.Get(),0);ctx->Unmap(readDepth.Get(),0);
    };
    mask.Replay(ctx.Get(),[&]{ctx->Draw(3,0);});verify();
    // A later rear surface fails the game's depth test and must not overwrite
    // the visible player's ownership/depth from the earlier front surface.
    ctx->VSSetShader(rear.Get(),nullptr,0);ctx->Draw(3,0);mask.Replay(ctx.Get(),[&]{ctx->Draw(3,0);});verify();
    ComPtr<ID3D11DepthStencilView> restored;ctx->OMGetRenderTargets(0,nullptr,&restored);Check(restored.Get()==dsv.Get(),"depth attachment restoration");
    ComPtr<ID3D11DepthStencilState> got;UINT reference;ctx->OMGetDepthStencilState(&got,&reference);Check(got.Get()==state.Get() && reference==37,"depth/stencil state restoration");
    std::printf("PASS %s biased %s %s: exact visible depth, rejected rear surface, state restored\n",driver==D3D_DRIVER_TYPE_HARDWARE?"hardware":"WARP",d24?"D24S8":"D32",reversed?"reversed":"normal");
}
int wmain(int argc,wchar_t** argv) {
    try {
        Check(argc>=2,"pass one or more CommunityShaders.dll paths");
        const bool matrix=std::wcscmp(argv[1],L"--matrix")==0;
        if(matrix)Check(argc==int(DapaCsxDraw::builds.size())+2,"--matrix requires exactly one DLL for every supported contract");
        std::array<bool,DapaCsxDraw::builds.size()> tested{};
        ScopeTest();for(const auto& build:DapaCsxDraw::builds)for(const auto& site:build.sites)InstructionTest(site);
        for(int i=matrix?2:1;i<argc;++i) {
            const auto* build=AdapterTest(argv[i]);
            const auto index=size_t(build-DapaCsxDraw::builds.data());
            if(matrix)Check(!tested[index],"duplicate DLL contract in matrix; another supported build is missing");
            tested[index]=true;
        }
        if(matrix) {
            for(bool covered:tested)Check(covered,"supported DLL missing from matrix");
            std::printf("PASS complete compatibility matrix: %zu distinct contracts\n",tested.size());
        }
        GpuTest(D3D_DRIVER_TYPE_WARP);GpuTest(D3D_DRIVER_TYPE_HARDWARE);
        for(auto driver:{D3D_DRIVER_TYPE_WARP,D3D_DRIVER_TYPE_HARDWARE})for(bool reversed:{false,true})for(bool d24:{false,true})BiasedDepthTest(driver,reversed,d24);
        return 0;
    } catch(const std::exception& error) {std::printf("FAIL: %s\n",error.what());return 1;}
}
