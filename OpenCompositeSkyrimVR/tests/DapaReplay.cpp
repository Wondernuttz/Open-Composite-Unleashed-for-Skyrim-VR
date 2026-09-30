// Offline D3D11 replay of recorded input, depth and constants. Never attaches to Skyrim.
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <json/json.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <array>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <chrono>
namespace LegacyShader {
#include "fixtures/DapaWarpPreBoundary.h"
}
using Microsoft::WRL::ComPtr;namespace fs=std::filesystem;
void HR(HRESULT h){if(FAILED(h)){char s[64];sprintf_s(s,"HRESULT %08x",unsigned(h));throw std::runtime_error(s);}}
struct Picture{UINT w=0,h=0;std::vector<unsigned char> bytes;};
Picture Load(IWICImagingFactory* f,const fs::path& p){
 ComPtr<IWICBitmapDecoder>d;HR(f->CreateDecoderFromFilename(p.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&d));
 ComPtr<IWICBitmapFrameDecode>frame;HR(d->GetFrame(0,&frame));ComPtr<IWICFormatConverter>c;HR(f->CreateFormatConverter(&c));
 HR(c->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
 Picture out;HR(c->GetSize(&out.w,&out.h));out.bytes.resize(size_t(out.w)*out.h*4);HR(c->CopyPixels(nullptr,out.w*4,UINT(out.bytes.size()),out.bytes.data()));return out;
}
void Save(IWICImagingFactory*f,const fs::path&p,Picture image){
 ComPtr<IWICStream>s;HR(f->CreateStream(&s));HR(s->InitializeFromFilename(p.c_str(),GENERIC_WRITE));
 ComPtr<IWICBitmapEncoder>e;HR(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&e));HR(e->Initialize(s.Get(),WICBitmapEncoderNoCache));
 ComPtr<IWICBitmapFrameEncode>frame;ComPtr<IPropertyBag2>props;HR(e->CreateNewFrame(&frame,&props));HR(frame->Initialize(props.Get()));HR(frame->SetSize(image.w,image.h));
 WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;HR(frame->SetPixelFormat(&format));
 if(format==GUID_WICPixelFormat32bppBGRA)for(size_t i=0;i<image.bytes.size();i+=4)std::swap(image.bytes[i],image.bytes[i+2]);
 else if(format!=GUID_WICPixelFormat32bppRGBA)throw std::runtime_error("PNG format");
 HR(frame->WritePixels(image.h,image.w*4,UINT(image.bytes.size()),image.bytes.data()));HR(frame->Commit());HR(e->Commit());
}
Picture Preview(const Picture&p){Picture o;o.w=std::min(1400u,p.w);o.h=UINT(uint64_t(o.w)*p.h/p.w);o.bytes.resize(size_t(o.w)*o.h*4);
 for(UINT y=0;y<o.h;++y)for(UINT x=0;x<o.w;++x)memcpy(&o.bytes[(size_t(y)*o.w+x)*4],&p.bytes[(size_t(uint64_t(y)*p.h/o.h)*p.w+uint64_t(x)*p.w/o.w)*4],4);return o;}
void Flip(Picture&p){const size_t pitch=p.w*4;for(UINT y=0;y<p.h/2;++y)for(size_t x=0;x<pitch;++x)std::swap(p.bytes[size_t(y)*pitch+x],p.bytes[size_t(p.h-1-y)*pitch+x]);}
int wmain(int argc,wchar_t**argv){try{
 if(argc<4)throw std::runtime_error("capture-folder output-folder shader-header [--hardware] [--baseline shader-header]");
 bool hardware=false;fs::path baselinePath;
 for(int i=4;i<argc;++i){const std::wstring arg=argv[i];if(arg==L"--hardware")hardware=true;else if(arg==L"--baseline"&&i+1<argc)baselinePath=argv[++i];else throw std::runtime_error("unknown/missing replay option");}
 const fs::path input=argv[1],output=argv[2];
 if(fs::exists(output))throw std::runtime_error("Output must be new");fs::create_directories(output);
 std::ifstream jf(input/"metadata.json");Json::Value meta;jf>>meta;
 std::ifstream sf{fs::path(argv[3])};std::string header((std::istreambuf_iterator<char>(sf)),{});
 const auto a=header.find("R\"HLSL(")+7,b=header.rfind(")HLSL\"");if(a<7||b<=a)throw std::runtime_error("shader header");const auto source=header.substr(a,b-a);
 std::string baselineSource=LegacyShader::s_warpShaderHLSL;
 if(!baselinePath.empty()) {std::ifstream bf(baselinePath);std::string text((std::istreambuf_iterator<char>(bf)),{});
  const auto begin=text.find("R\"HLSL(")+7,end=text.rfind(")HLSL\"");if(begin<7||end<=begin)throw std::runtime_error("baseline shader header");baselineSource=text.substr(begin,end-begin);}
 HR(CoInitializeEx(nullptr,COINIT_MULTITHREADED));struct ComEnd{~ComEnd(){CoUninitialize();}}end;
 ComPtr<IWICImagingFactory>factory;HR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
 ComPtr<ID3D11Device>device;ComPtr<ID3D11DeviceContext>ctx;D3D_FEATURE_LEVEL level;
 HR(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx));
 std::ofstream report(output/"metrics.txt");report<<(hardware?"Hardware isolated shader replay; not end-to-end game FPS.":"Software WARP replay. Not GPU performance.")<<" Next-frame difference is not ground truth.\n";
 for(int eye=0;eye<2;++eye){const std::string suffix=eye?"right":"left";const auto&m=meta["eyes"][eye];
  auto real=Load(factory.Get(),input/("real-"+suffix+".png")),recorded=Load(factory.Get(),input/("prediction-"+suffix+".png"));
  const UINT dw=m["depthWidth"].asUInt(),dh=m["depthHeight"].asUInt();std::vector<float>depth(size_t(dw)*dh);
  std::ifstream df(input/("depth-"+suffix+".f32"),std::ios::binary);if(!df.read((char*)depth.data(),depth.size()*4))throw std::runtime_error("depth data missing");
  if(m["sourceFlipV"].asBool()){Flip(real);for(UINT y=0;y<dh/2;++y)for(UINT x=0;x<dw;++x)std::swap(depth[size_t(y)*dw+x],depth[size_t(dh-1-y)*dw+x]);}
  std::array<float,68>constants{};for(unsigned i=0;i<68;++i)constants[i]=m["warpConstants"][i].asFloat();constants[27]=0;
  D3D11_TEXTURE2D_DESC td{};td.Width=real.w;td.Height=real.h;td.MipLevels=td.ArraySize=1;td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA data{real.bytes.data(),real.w*4,0};ComPtr<ID3D11Texture2D>color,dep,out,read;
  HR(device->CreateTexture2D(&td,&data,&color));td.BindFlags=D3D11_BIND_UNORDERED_ACCESS;HR(device->CreateTexture2D(&td,nullptr,&out));
  td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;HR(device->CreateTexture2D(&td,nullptr,&read));
  td.Width=dw;td.Height=dh;td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;td.Usage=D3D11_USAGE_DEFAULT;td.CPUAccessFlags=0;data={depth.data(),dw*4,0};HR(device->CreateTexture2D(&td,&data,&dep));
  ComPtr<ID3D11ShaderResourceView>sc,sd,bodySrv;ComPtr<ID3D11Texture2D>bodyTex;
  if(constants[65]>=3) {
   std::vector<float> body(size_t(dw)*dh);std::ifstream file(input/("player-mask-"+suffix+".f32"),std::ios::binary);
   if(!file.read((char*)body.data(),body.size()*4))throw std::runtime_error("active body mask missing; refusing inaccurate replay");
   if(m["sourceFlipV"].asBool())for(UINT y=0;y<dh/2;++y)for(UINT x=0;x<dw;++x)std::swap(body[size_t(y)*dw+x],body[size_t(dh-1-y)*dw+x]);
   data={body.data(),dw*4,0};HR(device->CreateTexture2D(&td,&data,&bodyTex));HR(device->CreateShaderResourceView(bodyTex.Get(),nullptr,&bodySrv));
  }
  ComPtr<ID3D11UnorderedAccessView>uav;HR(device->CreateShaderResourceView(color.Get(),nullptr,&sc));HR(device->CreateShaderResourceView(dep.Get(),nullptr,&sd));HR(device->CreateUnorderedAccessView(out.Get(),nullptr,&uav));
  ComPtr<ID3D11Buffer>cb;D3D11_BUFFER_DESC bd{};bd.ByteWidth=272;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;data={constants.data(),0,0};HR(device->CreateBuffer(&bd,&data,&cb));
  ComPtr<ID3D11SamplerState>sampler;D3D11_SAMPLER_DESC sam{};sam.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sam.AddressU=sam.AddressV=sam.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sam.MaxLOD=D3D11_FLOAT32_MAX;HR(device->CreateSamplerState(&sam,&sampler));
  Picture baseline;
  for(int variant=0;variant<2;++variant){ComPtr<ID3DBlob>code,error;
   const std::string shaderSource=variant?source:baselineSource;
   HRESULT h=D3DCompile(shaderSource.data(),shaderSource.size(),"Replay",nullptr,nullptr,"CSMain","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error);
   if(FAILED(h)&&error)fprintf(stderr,"%s",(char*)error->GetBufferPointer());HR(h);ComPtr<ID3D11ComputeShader>shader;HR(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader));
   ID3D11ShaderResourceView*srvs[]={sc.Get(),bodySrv.Get(),sd.Get()};ID3D11UnorderedAccessView*uavs[]={uav.Get()};ID3D11Buffer*buffers[]={cb.Get()};ID3D11SamplerState*sams[]={sampler.Get()};
   ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetShaderResources(0,3,srvs);ctx->CSSetUnorderedAccessViews(0,1,uavs,nullptr);ctx->CSSetConstantBuffers(0,1,buffers);ctx->CSSetSamplers(0,1,sams);ctx->Dispatch((real.w+7)/8,(real.h+7)/8,1);
   if(hardware){ComPtr<ID3D11Query>q,qa,qb;D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT,0};HR(device->CreateQuery(&qd,&q));qd.Query=D3D11_QUERY_TIMESTAMP;HR(device->CreateQuery(&qd,&qa));HR(device->CreateQuery(&qd,&qb));
    ctx->Begin(q.Get());ctx->End(qa.Get());for(int repeat=0;repeat<40;++repeat)ctx->Dispatch((real.w+7)/8,(real.h+7)/8,1);ctx->End(qb.Get());ctx->End(q.Get());ctx->Flush();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(ctx->GetData(q.Get(),&timing,sizeof(timing),0)==S_FALSE){if(std::chrono::steady_clock::now()>deadline)throw std::runtime_error("offline timing timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    UINT64 a=0,b=0;HR(ctx->GetData(qa.Get(),&a,sizeof(a),0));HR(ctx->GetData(qb.Get(),&b,sizeof(b),0));if(!timing.Disjoint)report<<(variant?"candidate-":"baseline-")<<suffix<<" mean GPU ms="<<double(b-a)*1000/timing.Frequency/40<<"\n";
   }
   ID3D11UnorderedAccessView*nu[]={nullptr};ID3D11ShaderResourceView*ns[3]{};ctx->CSSetUnorderedAccessViews(0,1,nu,nullptr);ctx->CSSetShaderResources(0,3,ns);ctx->CopyResource(read.Get(),out.Get());
   Picture result;result.w=real.w;result.h=real.h;result.bytes.resize(real.bytes.size());D3D11_MAPPED_SUBRESOURCE mapped{};HR(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
   for(UINT y=0;y<real.h;++y)memcpy(result.bytes.data()+size_t(y)*real.w*4,(char*)mapped.pData+size_t(y)*mapped.RowPitch,real.w*4);ctx->Unmap(read.Get(),0);
   double mae=0;size_t mismatches=0;for(size_t i=0;i<result.bytes.size();i+=4){int diff=0;for(int c=0;c<3;++c){const int d=std::abs(int(result.bytes[i+c])-int(recorded.bytes[i+c]));mae+=d;diff=std::max(diff,d);}mismatches+=diff>2;}
   const std::string name=(variant?"candidate-":"baseline-")+suffix;report<<name<<" recorded RGB MAE="<<mae/(real.w*real.h*3)<<" pixels >2 levels="<<100.*mismatches/(real.w*real.h)<<"%\n";
   if(variant){size_t changed=0,interior=0;for(UINT y=0;y<real.h;++y)for(UINT x=0;x<real.w;++x){const size_t i=(size_t(y)*real.w+x)*4;
    if(memcmp(result.bytes.data()+i,baseline.bytes.data()+i,3)){++changed;if(x>real.w*.12&&x<real.w*.88&&y>real.h*.12&&y<real.h*.88)++interior;}}
    report<<name<<" vs baseline changed pixels="<<changed<<" changed central 76% rectangle pixels="<<interior<<"\n";}
   Save(factory.Get(),output/(name+".png"),result);Save(factory.Get(),output/(name+"-preview.png"),Preview(result));
   if(!variant)baseline=result;
  }
 }
 puts("Replay saved; inspect metrics.txt and baseline/candidate PNGs.");return 0;
}catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
