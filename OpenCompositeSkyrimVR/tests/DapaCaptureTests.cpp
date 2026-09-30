#include "DrvOpenXR/DapaCapture.h"
#include "DrvOpenXR/DapaMotion.h"
#include "DrvOpenXR/DapaCaptureControl.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <wincodec.h>
using Microsoft::WRL::ComPtr;
static void Check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
static void HR(HRESULT h){Check(SUCCEEDED(h),"D3D/WIC call failed");}
void oovr_log_raw(const char*,long,const char*,const char* s){std::puts(s);}
void oovr_log_raw_format(const char*,long,const char*,const char* fmt,...){va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);std::puts("");}
static void Identity(float* m){std::fill(m,m+16,0.0f);for(int i=0;i<4;++i)m[i*4+i]=1;}
int wmain(int argc,wchar_t** argv) {
    try {
        DapaCaptureControl::GripLatch grip;
        Check(!grip.Update(true,true,true,100),"held grips on startup must not trigger");
        Check(!grip.Update(true,false,false,110),"release arms gesture");
        Check(!grip.Update(true,true,false,120),"one grip must not trigger");
        Check(!grip.Update(true,true,true,130),"both grips start debounce");
        Check(!grip.Update(true,true,true,200),"brief overlap must not trigger");
        Check(grip.Update(true,true,true,280),"both grips trigger after 150ms");
        Check(!grip.Update(true,true,true,450),"holding must not repeat");
        Check(!grip.Update(true,true,false,460),"one-hand release must not rearm");
        Check(!grip.Update(true,true,true,650),"one-hand bounce must not toggle");
        grip.Update(true,false,false,660);grip.Update(true,true,true,670);
        Check(grip.Update(true,true,true,820),"second full squeeze toggles stop");
        grip.Update(false,false,false,830);
        Check(!grip.Update(true,true,true,840),"focus return while held must not toggle");
        grip.Update(true,false,false,850);grip.Update(true,true,true,860);
        Check(!grip.Update(true,true,true,2000),"stale input must not toggle");
        Check(!DapaCaptureControl::SessionLimitReached(119999,8,false),"photo budget does not stop movement session");
        Check(DapaCaptureControl::SessionLimitReached(120000,0,true),"120 second limit stops scheduling even if busy");
        Check(!DapaCaptureControl::SessionLimitReached(1000,4,false),"failed attempts cannot stop session early");
        Check(!DapaCaptureControl::PhotoBudgetReached(7,2),"photo below budget may retry");
        Check(DapaCaptureControl::PhotoBudgetReached(8,0)&&DapaCaptureControl::PhotoBudgetReached(3,3),"photo budget/failure circuit bounds capture overhead");
        DapaCaptureControl::FeedbackPulses pulses;
        Check(pulses.Update(true,1,1000),"start pulse");
        Check(!pulses.Update(true,0,1250),"start has only one pulse");
        Check(pulses.Update(true,2,2000),"stop first pulse");
        Check(!pulses.Update(true,0,2100),"stop silent gap");
        Check(pulses.Update(true,0,2250),"stop second pulse");
        Check(!pulses.Update(true,0,2500),"stop has exactly two pulses");
        Check(pulses.Update(true,3,3000)&&pulses.Update(true,0,3250)&&pulses.Update(true,0,3500),"error has three pulses");
        pulses.Update(true,2,4000);pulses.Update(false,0,4050);
        Check(!pulses.Update(true,0,4250),"focus loss cancels pending haptics");
        DapaCaptureTelemetry::Measurement measured;float view[16];Identity(view);
        measured.SamplePosition(1000000000,2000000000,{0,0,0});
        Check(!measured.latest.velocityValid&&!measured.latest.Stationary(),"first sample is unknown, not stationary");
        measured.SamplePosition(1020000000,2020000000,{0,0,-2});measured.SetView(0,view,1);
        Check(measured.latest.velocityValid && std::abs(measured.latest.speed-100)<0.001f && measured.latest.eyeVelocity[0].z< -99,"measured backward speed independent of predictor");
        measured.SamplePosition(1040000000,2040000000,{2,0,-2});measured.SetView(0,view,1);
        Check(measured.latest.eyeVelocity[0].x>99,"measured right strafe");
        measured.SamplePosition(1060000000,2060000000,{2,0,-2});measured.SetView(0,view,1);
        Check(measured.latest.Stationary(),"player stationary with no actor displacement");
        measured.SamplePosition(1080000000,2080000000,{2,0,0});view[10]=2;measured.SetView(0,view,1);
        Check(measured.latest.eyeVelocity[0].z>99 && measured.latest.eyeVelocity[0].z<101,"view scaling must not double measured speed");
        measured.SamplePosition(1100000000,2100000000,{200,0,0});
        Check(measured.latest.discontinuity&&!measured.latest.velocityValid&&!measured.latest.Stationary(),"teleport flagged, not a valid speed or stationary");
        measured.Reset();Check(!measured.latest.Stationary(),"reset is unavailable, not stationary");
        Check(argc==2,"supply an isolated output directory");
        const std::filesystem::path root=argv[1];
        Check(!std::filesystem::exists(root),"fixture directory must be new");
        std::filesystem::create_directories(root);
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level;
        HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&ctx));
        constexpr unsigned w=640,h=360;
        Check(DapaCapture::EstimateBytes(2688,2880,1792,1920)<DapaCapture::MaxBytes,"current VR sizes fit budget");
        Check(DapaCapture::EstimateBytes(4096,4096,4096,4096)>DapaCapture::MaxBytes,"oversized capture rejected before allocation");
        std::vector<uint8_t> bytes(w*h*4);
        auto scene=[&](int shift){
            for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x) {
                unsigned i=(y*w+x)*4;const int sx=int(x)+shift;
                bytes[i]=y<180?38:49;bytes[i+1]=y<180?63:63;bytes[i+2]=y<180?86:51;bytes[i+3]=255;
                if(sx>50&&sx<260&&y>55&&y<280){bytes[i]=144;bytes[i+1]=129;bytes[i+2]=98;if((sx/32)%2==0&&(y/32)%2==0){bytes[i]=70;bytes[i+1]=78;bytes[i+2]=78;}}
                if(sx>355&&sx<395&&y>160&&y<290){bytes[i]=196;bytes[i+1]=134;bytes[i+2]=94;}
                if((sx-375)*(sx-375)+(int(y)-140)*(int(y)-140)<400){bytes[i]=221;bytes[i+1]=177;bytes[i+2]=136;}
                if(x>485&&x<510&&y>205&&y<295){bytes[i]=76;bytes[i+1]=171;bytes[i+2]=199;}
                if((int(x)-498)*(int(x)-498)+(int(y)-190)*(int(y)-190)<144){bytes[i]=221;bytes[i+1]=177;bytes[i+2]=136;}
                if(sx>550&&sx<610&&y>110&&y<285&&(sx+int(y))%13<5){bytes[i]=69;bytes[i+1]=137;bytes[i+2]=71;}
            }
        };
        scene(0);
        D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{bytes.data(),w*4,0};
        ComPtr<ID3D11Texture2D> real[2],next[2],depth,output;
        ComPtr<ID3D11ShaderResourceView> realSRV[2],depthSRV;
        for(int e=0;e<2;++e){HR(device->CreateTexture2D(&d,&init,&real[e]));HR(device->CreateShaderResourceView(real[e].Get(),nullptr,&realSRV[e]));}
        scene(8);for(int e=0;e<2;++e)HR(device->CreateTexture2D(&d,&init,&next[e]));
        d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;HR(device->CreateTexture2D(&d,nullptr,&output));
        ComPtr<ID3D11UnorderedAccessView> outputUAV;HR(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputUAV));
        std::vector<float> depths(w*h,0.9090909f);d.Format=DXGI_FORMAT_R32_FLOAT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        init={depths.data(),w*4,0};HR(device->CreateTexture2D(&d,&init,&depth));HR(device->CreateShaderResourceView(depth.Get(),nullptr,&depthSRV));
        float constants[68]{};Identity(constants);constants[3]=0.125f;
        constants[16]=w;constants[17]=h;constants[18]=1;constants[19]=100;
        constants[24]=1;constants[25]=3;constants[27]=1; // game must remain red; capture must be clean
        constants[28]=w;constants[29]=h;constants[64]=1;
        constants[65]=3;
        constants[32]=constants[37]=1;constants[42]=100.0f/99;constants[43]=1;constants[46]=-100.0f/99;
        Check(DapaMotion::Inverse(constants+32,constants+48),"inverse projection");
        ComPtr<ID3D11Buffer> cb;D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof(constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        init={constants,0,0};HR(device->CreateBuffer(&bd,&init,&cb));
        ComPtr<ID3D11SamplerState> sampler;D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;HR(device->CreateSamplerState(&sd,&sampler));
        DapaCapture capture(root);capture.ToggleSession();
        Check(capture.Recording(),"grip session starts");capture.Request(0);
        const int64_t t=900000000000000001;float pose[7]={0,0,0,1,0,0,0},fov[4]={-0.8f,0.8f,0.8f,-0.8f};
        Check(!capture.ShaderReady(),"compile result is collected only by nonblocking poll");
        Check(!capture.BeginEye(0,ctx.Get(),real[0].Get(),depth.Get(),constants,t,t+11000000,false,pose,pose,fov),"pending compile skips capture, not game frame");
        const auto prepareDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(!capture.ShaderReady() && std::chrono::steady_clock::now()<prepareDeadline){capture.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(20));}
        Check(capture.ShaderReady(),"CPU-only asynchronous shader preparation completes");
        ComPtr<ID3D11ComputeShader> firstShader;
        Identity(view);measured.SamplePosition(t-20000000,3000000000,{0,0,0});
        measured.SamplePosition(t,3020000000,{0,0,-2});measured.SetView(0,view,1);measured.SetView(1,view,1);
        measured.latest.sticks.timeNs=3019000000;measured.latest.sticks.axes={0,-1,0,0};measured.latest.sticks.active={true,true,true,true};
        capture.ObserveMovement(measured.latest);
        for(int e=0;e<2;++e) {
            auto* shader=capture.BeginEye(e,ctx.Get(),real[e].Get(),depth.Get(),constants,t,t+11000000,false,pose,pose,fov,depth.Get());
            Check(shader!=nullptr,"capture variant compile and allocate");
            if(e==0)firstShader=shader;
            ID3D11UnorderedAccessView* uav[]={outputUAV.Get(),capture.Clean(e),capture.Diagnostic(e)};
            ID3D11ShaderResourceView* srv[]={realSRV[e].Get(),depthSRV.Get(),depthSRV.Get()};
            ID3D11Buffer* buffers[]={cb.Get()};ID3D11SamplerState* samplers[]={sampler.Get()};
            ctx->CSSetShader(shader,nullptr,0);ctx->CSSetUnorderedAccessViews(0,3,uav,nullptr);ctx->CSSetShaderResources(0,3,srv);
            ctx->CSSetConstantBuffers(0,1,buffers);ctx->CSSetSamplers(0,1,samplers);ctx->Dispatch(w/8,h/8+1,1);
            ID3D11UnorderedAccessView* nu[3]{};ID3D11ShaderResourceView* ns[3]{};
            ctx->CSSetUnorderedAccessViews(0,3,nu,nullptr);ctx->CSSetShaderResources(0,3,ns);
            capture.EndEye(e,ctx.Get());
        }
        capture.Submission(t+11000000,0);
        ID3D11Texture2D* pair[]={next[0].Get(),next[1].Get()};bool flips[2]{};
        measured.SamplePosition(t+22000000,3042000000,{0,0,-2});measured.SetView(0,view,1);measured.SetView(1,view,1);
        capture.ObserveMovement(measured.latest);capture.NextPair(ctx.Get(),pair,t+22000000,flips,pose,pose);
        Check(capture.NeedsPump(),"next real completes matched capture");
        capture.ToggleSession();
        Check(!capture.Recording() && capture.NeedsPump(),"stop preserves in-flight readback");
        capture.ToggleSession();Check(!capture.Recording(),"restart while saving is rejected");
        // Standalone WARP test has no game Present/xrEndFrame to submit commands.
        // This flush is TEST ONLY; production capture never flushes.
        ctx->Flush();
        D3D11_TEXTURE2D_DESC readDesc{};output->GetDesc(&readDesc);
        readDesc.Usage=D3D11_USAGE_STAGING;readDesc.BindFlags=0;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> gameRead;HR(device->CreateTexture2D(&readDesc,nullptr,&gameRead));
        ctx->CopyResource(gameRead.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE gamePixels{};
        HR(ctx->Map(gameRead.Get(),0,D3D11_MAP_READ,0,&gamePixels));
        Check(std::abs(int(static_cast<uint8_t*>(gamePixels.pData)[1])-38)<=1,"game output keeps configured tint");
        ctx->Unmap(gameRead.Get(),0);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(capture.Busy()&&std::chrono::steady_clock::now()<deadline) {
            capture.Pump(ctx.Get());capture.Poll();std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(!capture.Busy(),"writer completes");
        std::filesystem::path report;
        for(auto& e:std::filesystem::recursive_directory_iterator(root))
            if(e.path().filename()=="metadata.json")report=e.path().parent_path();
        Check(!report.empty()&&std::filesystem::exists(report/"index.html"),"HTML report");
        Check(std::filesystem::exists(report.parent_path()/"index.html"),"stopped session comparison index saved");
        std::ifstream sessionFile(report.parent_path()/"index.html");
        const std::string sessionText((std::istreambuf_iterator<char>(sessionFile)),{});
        Check(sessionText.find("1 complete stereo samples")!=std::string::npos,"session contains complete stereo sample");
        std::ifstream jsonFile(report/"metadata.json");const std::string json((std::istreambuf_iterator<char>(jsonFile)),{});
        Check(json.find("\"realTime\":\"900000000000000001\"")!=std::string::npos,"XR time preserved without JSON numeric rounding");
        Check(json.find("\"submissionKnown\":true")!=std::string::npos,"submission result recorded");
        Check(json.find("source-depth-player-mask-v3")!=std::string::npos && json.find("\"playerMaskActive\":true")!=std::string::npos,"player-mask version and active state recorded");
        Check(json.find("\"speedWorldUnitsPerSecond\":100")!=std::string::npos,"raw speed serialized");
        Check(json.find("\"playerStationary\":true")!=std::string::npos,"next measured stationary frame serialized");
        Check(json.find("\"leftX_leftY_rightX_rightY\":[0,-1,0,0]")!=std::string::npos,"backward physical stick recorded");
        Check(std::filesystem::exists(report.parent_path()/"movement-timeline.json") && std::filesystem::exists(report.parent_path()/"movement-timeline.csv"),"session movement timeline saved");
        size_t pngCount=0;for(auto& e:std::filesystem::directory_iterator(report))if(e.path().extension()==".png")++pngCount;
        Check(pngCount==30,"all stereo PNGs, player mask previews and prebuilt overlays written");
        Check(std::filesystem::exists(report/"player-mask-left.f32") && std::filesystem::exists(report/"player-mask-right.png"),"both raw player masks saved");
        Check(std::filesystem::exists(report/"preview-images.js"),"offline canvas-safe embedded previews");
        HR(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        ComPtr<IWICImagingFactory> factory;HR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
        ComPtr<IWICBitmapDecoder> decoder;HR(factory->CreateDecoderFromFilename((report/"prediction-left.png").c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnLoad,&decoder));
        ComPtr<IWICBitmapFrameDecode> frame;HR(decoder->GetFrame(0,&frame));UINT iw,ih;HR(frame->GetSize(&iw,&ih));Check(iw==w&&ih==h,"full-resolution PNG");
        ComPtr<IWICFormatConverter> converter;HR(factory->CreateFormatConverter(&converter));
        HR(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
        uint8_t pixel[4];WICRect rect{0,0,1,1};HR(converter->CopyPixels(&rect,4,4,pixel));
        Check(std::abs(int(pixel[1])-63)<=1,"clean capture must not contain game red tint");
        converter.Reset();frame.Reset();decoder.Reset();factory.Reset();CoUninitialize();
        // Rejected generations must never manufacture a valid-looking report.
        capture.Request(0);Check(capture.BeginEye(0,ctx.Get(),real[0].Get(),depth.Get(),constants,t,t+11000000,false,pose,pose,fov),"start invalidation test");
        capture.HistoryInvalidated();Check(!capture.Busy(),"history invalidation cancels incomplete pair");
        Check(capture.ShaderReady(),"aborting releases textures, not cached bytecode");
        capture.Request(0);
        auto* reused=capture.BeginEye(0,ctx.Get(),real[0].Get(),depth.Get(),constants,t,t+11000000,false,pose,pose,fov);
        Check(reused==firstShader.Get(),"same device shader reused after completed and aborted captures");
        capture.EndEye(0,ctx.Get());
        Check(capture.BeginEye(1,ctx.Get(),real[1].Get(),depth.Get(),constants,t,t+11000000,false,pose,pose,fov),"stale-next test stereo pair");
        capture.EndEye(1,ctx.Get());
        capture.NextPair(ctx.Get(),pair,t+300000000,flips,pose,pose);
        Check(!capture.Busy(),"250 ms stale-data guard preserved, not relaxed to hide compile stall");
        capture.ToggleSession();capture.Request(0);capture.ToggleSession();
        Check(!capture.Recording()&&!capture.Busy(),"stop before first sample cancels armed capture");
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        DapaCaptureControl::toggleRequested.store(true);capture.Poll();
        Check(capture.Recording()&&DapaCaptureControl::recording.load(),"input mailbox starts session on render thread");
        std::this_thread::sleep_for(std::chrono::milliseconds(110));
        DapaCaptureControl::toggleRequested.store(true);capture.Poll();
        Check(!capture.Recording()&&!DapaCaptureControl::recording.load(),"input mailbox stops session on render thread");
        std::ofstream marker(root/"SYNTHETIC-TEST-NOT-SKYRIM.txt");marker<<"Synthetic software-rendered fixture. No headset footage.";marker.close();
        std::printf("REPORT=%s\n",report.string().c_str());
        std::puts("PASS: distinct nonblocking haptic patterns, bounded 120s sessions, async shader warmup/cache reuse, stale-data guards, grip/focus, actual stereo capture shader, clean PNG/WIC and report.");
        return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
