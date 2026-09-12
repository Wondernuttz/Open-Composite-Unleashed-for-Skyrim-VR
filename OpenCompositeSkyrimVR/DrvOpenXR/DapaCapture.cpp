#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DapaCapture.h"
#include "DapaWarpShader.h"
#include "DapaCaptureViewer.h"
#include "DapaCaptureControl.h"
#include "../OpenOVR/logging.h"
#include <d3dcompiler.h>
#include <shlobj.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
static void Require(HRESULT hr,const char* what) {
    if(FAILED(hr)) { std::ostringstream s;s<<what<<" HRESULT=0x"<<std::hex<<unsigned(hr);throw std::runtime_error(s.str()); }
}
static void SaveBytes(const fs::path& p,const void* bytes,size_t size) {
    std::ofstream f(p,std::ios::binary);f.exceptions(std::ios::failbit|std::ios::badbit);
    f.write(static_cast<const char*>(bytes),std::streamsize(size));
}
static void SaveText(const fs::path& p,const std::string& text) { SaveBytes(p,text.data(),text.size()); }
static void MovementJson(std::ostream& out,const DapaCaptureTelemetry::Sample& m) {
    auto number=[&](double value){if(std::isfinite(value))out<<value;else out<<"null";};
    auto vec=[&](DapaMotion::Vec3 v){out<<'[';number(v.x);out<<',';number(v.y);out<<',';number(v.z);out<<']';};
    out<<"{\"positionValid\":"<<(m.positionValid?"true":"false")<<",\"velocityValid\":"<<(m.velocityValid?"true":"false")
        <<",\"playerStationary\":"<<(m.Stationary()?"true":"false")
        <<",\"discontinuity\":"<<(m.discontinuity?"true":"false")<<",\"xrTime\":\""<<m.xrTime<<"\",\"previousXrTime\":\""<<m.previousXrTime
        <<"\",\"cpuTimeNs\":\""<<m.cpuTimeNs<<"\",\"previousCpuTimeNs\":\""<<m.previousCpuTimeNs<<"\",\"cpuIntervalSeconds\":";
    number(m.cpuInterval);out<<",\"xrIntervalSeconds\":";number(m.xrInterval);
    out<<",\"position\":";vec(m.position);out<<",\"previousPosition\":";vec(m.previousPosition);out<<",\"delta\":";vec(m.delta);
    out<<",\"velocityWorldUnitsPerSecond\":";vec(m.velocityCpu);out<<",\"velocityPerXrSecond\":";vec(m.velocityXr);
    out<<",\"speedWorldUnitsPerSecond\":";number(m.speed);out<<",\"predictorVelocity\":";vec(m.predictorVelocity);
    out<<",\"predictorConfidence\":";number(m.predictorConfidence);
    out<<",\"actorYawRadians\":";number(m.actorYaw);
    out<<",\"backwardTurnRateRadiansPerSecond\":";number(m.turnRate);
    out<<",\"turnConfidence\":";number(m.turnConfidence);
    out<<",\"turnInput\":"<<(m.turnInput?"true":"false")<<",\"turnValid\":"<<(m.turnValid?"true":"false");
    out<<",\"eyeVelocityRightUpForward\":[";vec(m.eyeVelocity[0]);out<<',';vec(m.eyeVelocity[1]);
    out<<"],\"eyeVelocityValid\":["<<(m.eyeValid[0]?"true":"false")<<','<<(m.eyeValid[1]?"true":"false")<<"],\"viewMatrices\":[";
    for(int e=0;e<2;++e){if(e)out<<',';out<<'[';for(int i=0;i<16;++i){if(i)out<<',';number(m.views[e][i]);}out<<']';}
    out<<"],\"physicalStickInput\":{\"cpuTimeNs\":\""<<m.sticks.timeNs<<"\",\"ageMs\":";
    const bool fresh=m.sticks.timeNs>0 && m.cpuTimeNs>=m.sticks.timeNs && m.cpuTimeNs-m.sticks.timeNs<=250000000;
    if(m.sticks.timeNs>0)number(double(m.cpuTimeNs-m.sticks.timeNs)*1e-6);else out<<"null";
    out<<",\"fresh\":"<<(fresh?"true":"false")<<",\"leftX_leftY_rightX_rightY\":[";
    for(int i=0;i<4;++i){if(i)out<<',';number(m.sticks.axes[i]);}out<<"],\"active\":[";
    for(int i=0;i<4;++i){if(i)out<<',';out<<(m.sticks.active[i]?"true":"false");}out<<"]}}";
}
static std::string Base64(const std::string& input) {
    static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;out.reserve((input.size()+2)/3*4);
    for(size_t i=0;i<input.size();i+=3) {
        const unsigned a=uint8_t(input[i]),b=i+1<input.size()?uint8_t(input[i+1]):0,c=i+2<input.size()?uint8_t(input[i+2]):0;
        out+=alphabet[a>>2];out+=alphabet[((a&3)<<4)|(b>>4)];
        out+=i+1<input.size()?alphabet[((b&15)<<2)|(c>>6)]:'=';out+=i+2<input.size()?alphabet[c&63]:'=';
    }
    return out;
}
static void Png(IWICImagingFactory* factory,const fs::path& path,uint32_t w,uint32_t h,const uint8_t* bytes) {
    ComPtr<IWICStream> stream;Require(factory->CreateStream(&stream),"WIC stream");
    Require(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG file");
    ComPtr<IWICBitmapEncoder> encoder;Require(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");
    Require(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialize");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> options;
    Require(encoder->CreateNewFrame(&frame,&options),"PNG frame");Require(frame->Initialize(options.Get()),"PNG frame initialize");
    Require(frame->SetSize(w,h),"PNG size");WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;
    Require(frame->SetPixelFormat(&format),"PNG pixel format");
    std::vector<uint8_t> converted;
    if(format==GUID_WICPixelFormat32bppBGRA) {
        converted.assign(bytes,bytes+size_t(w)*h*4);
        for(size_t i=0;i<converted.size();i+=4)std::swap(converted[i],converted[i+2]);
        bytes=converted.data();
    } else if(format!=GUID_WICPixelFormat32bppRGBA) throw std::runtime_error("Unsupported WIC PNG pixel format");
    Require(frame->WritePixels(h,w*4,w*h*4,const_cast<BYTE*>(bytes)),"PNG pixels");
    Require(frame->Commit(),"PNG frame commit");Require(encoder->Commit(),"PNG commit");
}
bool DapaCapture::InitializePath() {
    if(!root.empty())return true;
    PWSTR documents=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,KF_FLAG_DEFAULT,nullptr,&documents))) return false;
    root=fs::path(documents)/L"My Games"/L"Skyrim VR"/L"DAPA Captures";CoTaskMemFree(documents);
    try { fs::create_directories(root); }
    catch(...) { root.clear();return false; }
    return true;
}
DapaCapture::~DapaCapture() {
    // DLL shutdown must not leave a detached worker executing unloaded code.
    if(compiler.valid()){try{compiler.get();}catch(...){}}
    if(writer.valid()) { try { auto path=writer.get();if(!sessionRoot.empty())sessionSamples.push_back(fs::path(path).filename().string()); } catch(...) {} }
    recording=false;state=State::Idle;
    DapaCaptureControl::recording.store(false);
    DapaCaptureControl::telemetryWanted.store(false);
    if(!sessionRoot.empty())FinishSession();
}
void DapaCapture::ToggleSession() {
    if constexpr (!DapaCaptureControl::Enabled) return;
    if(recording) { StopSession();return; }
    if(Busy() || !sessionRoot.empty()) {
        OOVR_LOG("DAPA CAPTURE: still saving; wait before starting another session");
        DapaCaptureControl::feedback.store(3);return;
    }
    if(!InitializePath()) { DapaCaptureControl::feedback.store(3);return; }
    try {
        SYSTEMTIME t;GetLocalTime(&t);wchar_t name[128];
        swprintf_s(name,L"%04u-%02u-%02u_%02u-%02u-%02u-%03u_%lu_movement_session",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
        auto folder=root/name;
        if(!fs::create_directory(folder))throw std::runtime_error("session directory already exists");
        movementTimeline.clear();movementTimeline.reserve(DapaCaptureControl::MaxMovementSamples);droppedMovementSamples=0;
        sessionRoot=folder;sessionSamples.clear();sessionAttempts=sessionFailures=0;photoBudgetReported=false;
        recording=true;sessionStarted=Clock::now();nextSample=sessionStarted+std::chrono::milliseconds(250);
        DapaCaptureControl::recording.store(true);DapaCaptureControl::telemetryWanted.store(true);DapaCaptureControl::feedback.store(1);
        OOVR_LOG("DAPA CAPTURE: session STARTED; one pulse=start, two=stop. Both grips again to stop; 120-second safety limit, at most 8 photo attempts. Movement continues after photo budget; not a benchmark.");
    } catch(const std::exception& e) { OOVR_LOGF("DAPA CAPTURE: cannot start: %s",e.what());DapaCaptureControl::feedback.store(3); }
}
void DapaCapture::StopSession() {
    if(!recording)return;
    recording=false;DapaCaptureControl::recording.store(false);DapaCaptureControl::feedback.store(2);
    if(state==State::Armed)Abort("session stopped before next sample");
    OOVR_LOG("DAPA CAPTURE: session STOPPED; finishing any in-flight sample and saving comparison index");
    if(!Busy())FinishSession();
}
void DapaCapture::FinishSession() {
    if(sessionRoot.empty())return;
    try {
        std::ostringstream timeline;timeline<<std::setprecision(9)<<"{\"units\":\"Skyrim world units per CPU wall-clock second, not calibrated metres\",\"source\":\"raw actor position on completed cached real stereo frames; not a full game simulation trace\",\"droppedSamples\":"<<droppedMovementSamples<<",\"samples\":[";
        bool first=true;for(const auto& sample:movementTimeline){if(!first)timeline<<',';first=false;MovementJson(timeline,sample);}timeline<<"]}";
        SaveText(sessionRoot/"movement-timeline.json",timeline.str());
        std::ostringstream csv;csv<<std::setprecision(9)<<"elapsed_ms,valid,discontinuity,speed_world_units_per_sec,right,up,forward,eye_direction_valid,left_stick_x,left_stick_y,right_stick_x,right_stick_y,stick_age_ms,predictor_confidence\n";
        for(const auto& sample:movementTimeline) {
            const auto v=sample.eyeVelocity[0];
            csv<<double(sample.cpuTimeNs-movementTimeline.front().cpuTimeNs)*1e-6<<','<<sample.velocityValid<<','<<sample.discontinuity<<',';
            if(sample.velocityValid)csv<<sample.speed;csv<<',';
            if(sample.eyeValid[0])csv<<v.x;csv<<',';if(sample.eyeValid[0])csv<<v.y;csv<<',';if(sample.eyeValid[0])csv<<v.z;
            csv<<','<<sample.eyeValid[0];
            const bool fresh=sample.sticks.timeNs>0 && sample.cpuTimeNs>=sample.sticks.timeNs && sample.cpuTimeNs-sample.sticks.timeNs<=250000000;
            for(int i=0;i<4;++i){csv<<',';if(fresh && sample.sticks.active[i])csv<<sample.sticks.axes[i];}
            csv<<',';if(sample.sticks.timeNs>0)csv<<double(sample.cpuTimeNs-sample.sticks.timeNs)*1e-6;
            csv<<','<<sample.predictorConfidence<<'\n';
        }
        SaveText(sessionRoot/"movement-timeline.csv",csv.str());
        std::ostringstream html;
        html<<"<!doctype html><meta charset='utf-8'><title>DAPA capture session</title>"
            "<style>body{background:#141920;color:#eee;font:18px system-ui;max-width:900px;margin:50px auto;padding:24px}a{color:#e2b142}li{margin:24px 0}</style>"
            "<h1>DAPA comparison photos</h1><p>Recording stopped. "<<sessionSamples.size()<<" complete stereo samples saved; "<<sessionAttempts<<" attempts, "<<sessionFailures<<" failed/cancelled.</p>"
            "<p>Each comparison includes the real image, clean DAPA prediction, next real image, depth and confidence. These are spaced snapshots, not continuous video. Capture overhead is not normal FPS.</p><ol>";
        for(size_t i=0;i<sessionSamples.size();++i)html<<"<li><a href='"<<sessionSamples[i]<<"/index.html'>Open comparison "<<i+1<<"</a> — "
            "<a href='"<<sessionSamples[i]<<"/overlay-original-left.png'>Left overlay PNG</a> / "
            "<a href='"<<sessionSamples[i]<<"/overlay-original-right.png'>Right overlay PNG</a></li>";
        html<<"</ol><p><a href='movement-timeline.csv'>Movement speed/direction timeline (CSV)</a> · <a href='movement-timeline.json'>Full raw movement data (JSON)</a></p>"
            "<p>"<<movementTimeline.size()<<" movement samples; "<<droppedMovementSamples<<" dropped at the diagnostic cap. Speed is Skyrim world units per wall-clock second. Positive forward = forward; negative = backward. Direction is relative to the captured game view. Physical sticks are before game remapping and are not proof of actual motion.</p>";
        if(sessionSamples.empty())html<<"<p>No complete sample. DAPA must be actively producing predictions; check DAPA CAPTURE entries in OCUnleashedSKSE.log.</p>";
        SaveText(sessionRoot/"index.html",html.str());
        SaveText(sessionRoot/"README.txt","Both-grips diagnostic session. Open index.html. Individual comparisons are in the subfolders. No continuous recording or uploads.\r\n");
        OOVR_LOGF("DAPA CAPTURE: session saved %s",sessionRoot.string().c_str());
    } catch(const std::exception& e) { OOVR_LOGF("DAPA CAPTURE: session index failed: %s",e.what()); }
    sessionRoot.clear();sessionSamples.clear();movementTimeline.clear();
}
void DapaCapture::ObserveMovement(const DapaCaptureTelemetry::Sample& sample) {
    latestMovement=sample;
    if(recording) {
        if(movementTimeline.size()<DapaCaptureControl::MaxMovementSamples)movementTimeline.push_back(sample);
        else ++droppedMovementSamples;
    }
}
void DapaCapture::ReleaseGpu() {
    // Keep the device shader and compiled bytecode across captures. Only transient
    // diagnostic textures are released here; no repeated compile in a display slot.
    for(int e=0;e<2;++e){cleanUav[e].Reset();diagnosticUav[e].Reset();clean[e].Reset();diagnostic[e].Reset();}
    for(auto& i:images)i.staging.Reset();
}
void DapaCapture::Abort(const char* reason) {
    if(state==State::Idle||state==State::Writing)return;
    OOVR_LOGF("DAPA CAPTURE: aborted (%s); no complete report saved",reason);
    if(recording)++sessionFailures;
    ReleaseGpu();images.clear();state=State::Idle;eyeMask=0;bytesAllocated=0;
}
void DapaCapture::Request(unsigned delayMs) {
    if constexpr (!DapaCaptureControl::Enabled) return;
    if(Busy()){OOVR_LOG("DAPA CAPTURE: request ignored; capture/writer already busy");return;}
    if(!InitializePath()){OOVR_LOG("DAPA CAPTURE: Documents output directory unavailable");return;}
    if(shaderCode.empty() && !compiler.valid()) {
        compiler=std::async(std::launch::async,[] {
            ComPtr<ID3DBlob> code,error;
            D3D_SHADER_MACRO defines[]={{"DAPA_CAPTURE","1"},{nullptr,nullptr}};
            Require(D3DCompile(s_warpShaderHLSL,strlen(s_warpShaderHLSL),"DapaCaptureWarp",defines,nullptr,
                "CSMain","cs_5_0",D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&error),"capture shader compile");
            const auto* begin=static_cast<const uint8_t*>(code->GetBufferPointer());
            return std::vector<uint8_t>(begin,begin+code->GetBufferSize());
        }); // CPU-only worker; no D3D device/context calls or render-thread wait
    }
    armedAt=Clock::now()+std::chrono::milliseconds(std::min(delayMs,10000u));
    started=Clock::now();state=State::Armed;
    OOVR_LOGF("DAPA CAPTURE: armed, delay=%ums; one real/predicted/next-real stereo sample. Capture overhead is NOT benchmark timing.",std::min(delayMs,10000u));
}
void DapaCapture::Poll() {
    if constexpr (!DapaCaptureControl::Enabled) return;
    const auto now=Clock::now();
    if(now-lastPoll<std::chrono::milliseconds(100))return;
    lastPoll=now;
    if(compiler.valid() && compiler.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try{shaderCode=compiler.get();OOVR_LOG("DAPA CAPTURE: diagnostic shader prepared asynchronously; reused for later samples");}
        catch(const std::exception& e){Abort(e.what());DapaCaptureControl::feedback.store(3);}
    }
    if(state==State::Writing && writer.valid() && writer.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try { const auto result=writer.get();OOVR_LOGF("DAPA CAPTURE: saved %s (open index.html)",result.c_str());
            if(!sessionRoot.empty())sessionSamples.push_back(fs::path(result).filename().string()); }
        catch(const std::exception& e){OOVR_LOGF("DAPA CAPTURE: writer failed: %s",e.what());}
        state=State::Idle;
        nextSample=now+std::chrono::seconds(5);
    }
    if((state==State::Armed && now-started>std::chrono::seconds(15))
        ||((state==State::Warping||state==State::Next||state==State::Reading)&&now-started>std::chrono::seconds(10)))
        Abort("timed out waiting for a matched stereo sample/readback");
    if(DapaCaptureControl::toggleRequested.exchange(false))ToggleSession();
    if(recording && DapaCaptureControl::SessionLimitReached(
        now>sessionStarted?uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now-sessionStarted).count()):0,
        sessionAttempts,Busy()))StopSession();
    const bool photoBudget=DapaCaptureControl::PhotoBudgetReached(sessionAttempts,sessionFailures);
    if(recording && photoBudget && !Busy() && !photoBudgetReported) {
        photoBudgetReported=true;
        OOVR_LOG("DAPA CAPTURE: photo attempts paused (budget/repeated failures); MOVEMENT RECORDING CONTINUES until both grips or 120 seconds");
        if(sessionFailures>=3)DapaCaptureControl::feedback.store(3);
    }
    if(recording && !Busy() && !photoBudget && now>=nextSample) {
        ++sessionAttempts;Request(0);nextSample=now+std::chrono::seconds(5);
    }
    if(!recording && !Busy() && !sessionRoot.empty())FinishSession();
    DapaCaptureControl::telemetryWanted.store(recording || Busy());
    DWORD process=0;GetWindowThreadProcessId(GetForegroundWindow(),&process);
    const bool pressed=process==GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL)&0x8000)
        && (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState(VK_F10)&0x8000);
    if(pressed&&!keyDown && sessionRoot.empty())Request();
    keyDown=pressed;
    if(now-lastFilePoll<std::chrono::milliseconds(500))return;
    lastFilePoll=now;
    if(!InitializePath())return;
    // A fixed local control file lets the desktop test helper request capture.
    // Its contents are never interpreted as paths, commands or code.
    std::error_code ec;
    const auto request=root/L"capture.request";
    if(fs::is_regular_file(request,ec)) {
        fs::remove(request,ec);
        if(!ec && sessionRoot.empty())Request();
    }
}
void DapaCapture::Allocate(ID3D11Device* device,ID3D11Texture2D* source,Image& image) {
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    if(d.SampleDesc.Count!=1 || d.ArraySize!=1 || d.MipLevels!=1
        || (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R32_FLOAT))
        throw std::runtime_error("unsupported capture source format/layout");
    const uint64_t bytes=uint64_t(d.Width)*d.Height*4;
    if(bytesAllocated+bytes>MaxBytes)throw std::runtime_error("512 MiB capture allocation budget exceeded");
    bytesAllocated+=bytes;image.width=d.Width;image.height=d.Height;image.depth=d.Format==DXGI_FORMAT_R32_FLOAT;
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    Require(device->CreateTexture2D(&d,nullptr,&image.staging),"capture staging texture");
}
void DapaCapture::Queue(ID3D11DeviceContext* ctx,ID3D11Texture2D* source,Image& image) {
    ctx->CopyResource(image.staging.Get(),source);image.queued=true;
}
ID3D11ComputeShader* DapaCapture::BeginEye(int eye,ID3D11DeviceContext* ctx,
    ID3D11Texture2D* real,ID3D11Texture2D* depth,const float* constants,
    int64_t realTime,int64_t predictedTime,bool flip,const float* cachedPose,const float* targetPose,const float* fov,ID3D11Texture2D* bodyMask) {
	if constexpr (!DapaCaptureControl::Enabled) return nullptr;
    if(state==State::Armed && eye==0 && Clock::now()>=armedAt) {
        if(shaderCode.empty())return nullptr; // normal game warp continues during CPU compilation
        try {
            D3D11_TEXTURE2D_DESC colorDesc{},depthDesc{};real->GetDesc(&colorDesc);depth->GetDesc(&depthDesc);
            if(EstimateBytes(colorDesc.Width,colorDesc.Height,depthDesc.Width,depthDesc.Height)>MaxBytes)
                throw std::runtime_error("capture exceeds 512 MiB texture budget (no textures allocated)");
            if(realTime<=0 || predictedTime<=realTime)throw std::runtime_error("real/predicted timestamps unavailable");
            ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
            if(!shader || shaderDevice.Get()!=device.Get()) {
                shader.Reset();shaderDevice.Reset();
                Require(device->CreateComputeShader(shaderCode.data(),shaderCode.size(),nullptr,&shader),"capture shader");
                shaderDevice=device;
            }
            images.clear();images.resize(10);metadata={};eyeMask=0;bytesAllocated=0;readingIndex=0;
            metadata.realTime=realTime;metadata.predictedTime=predictedTime;
            if(latestMovement.xrTime==realTime)metadata.movement=latestMovement;
            state=State::Warping;started=Clock::now();
        } catch(const std::exception& e){Abort(e.what());return nullptr;}
    }
    if(state!=State::Warping)return nullptr;
    if(eye<0||eye>1||eyeMask!=(eye==0?0:1)||realTime!=metadata.realTime||predictedTime!=metadata.predictedTime){
        Abort("stereo generation/time mismatch");return nullptr;
    }
    try {
        ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
        const int base=eye*5;const std::string suffix=eye==0?"-left":"-right";
        const char* names[]={"real","prediction","diagnostic","depth","next"};
        for(int i=0;i<5;++i)images[base+i].name=names[i]+suffix;
        for(int i:{0,1,2,4})Allocate(device.Get(),real,images[base+i]);
        Allocate(device.Get(),depth,images[base+3]);
        D3D11_TEXTURE2D_DESC d{};real->GetDesc(&d);d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;d.CPUAccessFlags=0;d.MiscFlags=0;d.Usage=D3D11_USAGE_DEFAULT;
        const uint64_t extra=uint64_t(d.Width)*d.Height*8;
        if(bytesAllocated+extra>MaxBytes)throw std::runtime_error("512 MiB capture allocation budget exceeded");
        bytesAllocated+=extra;
        Require(device->CreateTexture2D(&d,nullptr,&clean[eye]),"clean capture texture");
        Require(device->CreateTexture2D(&d,nullptr,&diagnostic[eye]),"diagnostic texture");
        Require(device->CreateUnorderedAccessView(clean[eye].Get(),nullptr,&cleanUav[eye]),"clean capture UAV");
        Require(device->CreateUnorderedAccessView(diagnostic[eye].Get(),nullptr,&diagnosticUav[eye]),"diagnostic capture UAV");
        images[base].flip=images[base+3].flip=flip;
        metadata.flip[eye]=flip;
        std::copy(constants,constants+68,metadata.constants[eye].begin());
        std::copy(cachedPose,cachedPose+7,metadata.cachedPose[eye].begin());
        std::copy(targetPose,targetPose+7,metadata.targetPose[eye].begin());
        std::copy(fov,fov+4,metadata.fov[eye].begin());
        Queue(ctx,real,images[base]);Queue(ctx,depth,images[base+3]);
        if(bodyMask) {
            Image mask;mask.name="player-mask"+suffix;mask.bodyMask=true;mask.flip=flip;
            Allocate(device.Get(),bodyMask,mask);Queue(ctx,bodyMask,mask);images.push_back(std::move(mask));
        }
        return shader.Get();
    } catch(const std::exception& e){Abort(e.what());return nullptr;}
}
void DapaCapture::EndEye(int eye,ID3D11DeviceContext* ctx) {
    if(state!=State::Warping)return;
    Queue(ctx,clean[eye].Get(),images[eye*5+1]);Queue(ctx,diagnostic[eye].Get(),images[eye*5+2]);
    eyeMask|=uint8_t(1<<eye);
    if(eyeMask==3) {state=State::Next;OOVR_LOG("DAPA CAPTURE: real + prediction copied; waiting for next real stereo pair");}
}
void DapaCapture::Submission(int64_t time,int result) {
    if((state==State::Next||state==State::Reading)&&time==metadata.predictedTime) {
        metadata.submissionKnown=true;metadata.submissionResult=result;
    }
}
void DapaCapture::NextPair(ID3D11DeviceContext* ctx,ID3D11Texture2D* const* color,int64_t time,
    const bool* flips,const float* leftPose,const float* rightPose) {
    if(state!=State::Next)return;
    if(time<=metadata.predictedTime || time-metadata.realTime>250000000) {
        OOVR_LOGF("DAPA CAPTURE: invalid next timestamps real=%lld predicted=%lld next=%lld realAge=%.2fms afterPrediction=%.2fms",
            (long long)metadata.realTime,(long long)metadata.predictedTime,(long long)time,
            double(time-metadata.realTime)*1e-6,double(time-metadata.predictedTime)*1e-6);
        Abort("next real frame is stale/out of order");return;
    }
    for(int eye=0;eye<2;++eye) {
        D3D11_TEXTURE2D_DESC d{};color[eye]->GetDesc(&d);
        auto& next=images[eye*5+4];
        if(d.Width!=next.width||d.Height!=next.height||d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM||flips[eye]!=metadata.flip[eye]) {
            Abort("dimensions/orientation changed before next real frame");return;
        }
    }
    metadata.nextTime=time;
    if(latestMovement.xrTime==time)metadata.nextMovement=latestMovement;
    std::copy(leftPose,leftPose+7,metadata.nextPose[0].begin());
    std::copy(rightPose,rightPose+7,metadata.nextPose[1].begin());
    for(int eye=0;eye<2;++eye) {
        images[eye*5+4].flip=flips[eye];Queue(ctx,color[eye],images[eye*5+4]);
    }
    state=State::Reading;readingIndex=0;
}
void DapaCapture::Pump(ID3D11DeviceContext* ctx) {
    if(state!=State::Reading)return;
    try {
        auto& image=images[readingIndex];
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr=ctx->Map(image.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING)return; // no wait, flush, query spin or fence
        Require(hr,"capture readback");
        try {
            image.bytes.resize(size_t(image.width)*image.height*4);
            for(uint32_t y=0;y<image.height;++y)
                memcpy(image.bytes.data()+size_t(y)*image.width*4,
                    static_cast<const uint8_t*>(mapped.pData)+size_t(image.flip?image.height-1-y:y)*mapped.RowPitch,image.width*4);
        } catch(...) {ctx->Unmap(image.staging.Get(),0);throw;}
        ctx->Unmap(image.staging.Get(),0);image.staging.Reset();image.ready=true;
        if(++readingIndex==images.size()) {
            ReleaseGpu();
            writer=std::async(std::launch::async,&DapaCapture::Write,sessionRoot.empty()?root:sessionRoot,std::move(images),metadata);
            state=State::Writing;
            OOVR_LOG("DAPA CAPTURE: GPU readback complete; PNG/report writing on CPU worker");
        }
    } catch(const std::exception& e){Abort(e.what());}
}

std::string DapaCapture::Write(fs::path root,std::vector<Image> images,Metadata meta) {
    Require(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"capture worker COM");
    struct Uninit {~Uninit(){CoUninitialize();}} uninit;
    ComPtr<IWICImagingFactory> factory;
    Require(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    SYSTEMTIME t;GetLocalTime(&t);
    wchar_t name[128];swprintf_s(name,L"%04u-%02u-%02u_%02u-%02u-%02u-%03u_%lu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
    fs::path folder=root/name;
    if(!fs::create_directory(folder))throw std::runtime_error("capture output already exists");
    std::ostringstream previews;previews<<"window.captureImages={";bool firstPreview=true;
    auto preview=[&](const Image& image,const uint8_t* data){
        const uint32_t w=std::min(image.width,1600u),h=std::max(1u,uint32_t(uint64_t(image.height)*w/image.width));
        std::vector<uint8_t> previewPixels(size_t(w)*h*4);
        for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
            const auto sx=std::min(image.width-1,uint32_t((uint64_t(x)*2+1)*image.width/(2*w)));
            const auto sy=std::min(image.height-1,uint32_t((uint64_t(y)*2+1)*image.height/(2*h)));
            memcpy(previewPixels.data()+(size_t(y)*w+x)*4,data+(size_t(sy)*image.width+sx)*4,4);
        }
        const auto path=folder/("preview-"+image.name+".png");Png(factory.Get(),path,w,h,previewPixels.data());
        std::ifstream file(path,std::ios::binary);file.exceptions(std::ios::badbit);
        const std::string png((std::istreambuf_iterator<char>(file)),{});
        if(!firstPreview)previews<<",";firstPreview=false;
        previews<<"\""<<image.name<<"\":\"data:image/png;base64,"<<Base64(png)<<"\"";
    };
    for(size_t index=0;index<images.size();++index) {
        auto& image=images[index];
        if(!image.ready)throw std::runtime_error("incomplete capture refused");
        if(image.bodyMask) {
            SaveBytes(folder/(image.name+".f32"),image.bytes.data(),image.bytes.size());
            std::vector<uint8_t> pixels(image.bytes.size());
            const auto& scene=images[image.name=="player-mask-right"?8:3];
            if(scene.bytes.size()!=image.bytes.size())throw std::runtime_error("player mask/depth size mismatch");
            for(size_t i=0;i<pixels.size();i+=4) {
                float d,z;memcpy(&d,image.bytes.data()+i,4);memcpy(&z,scene.bytes.data()+i,4);
                pixels[i]=pixels[i+1]=pixels[i+2]=(std::isfinite(d)&&std::isfinite(z)&&d>=0&&d<=1&&std::abs(d-z)<=.000002f)?255:0;pixels[i+3]=255;
            }
            Png(factory.Get(),folder/(image.name+".png"),image.width,image.height,pixels.data());preview(image,pixels.data());
        } else if(image.depth) {
            SaveBytes(folder/(image.name+".f32"),image.bytes.data(),image.bytes.size());
            std::vector<uint8_t> depthPreview(image.bytes.size());
            const auto& cb=meta.constants[index/5];
            for(uint32_t y=0;y<image.height;++y)for(uint32_t x=0;x<image.width;++x) {
                const size_t pos=(size_t(y)*image.width+x)*4;float depth;
                memcpy(&depth,image.bytes.data()+pos,4);
                const float nx=2*(x+0.5f)/image.width-1;
                const float rawY=meta.flip[index/5]?1-(y+0.5f)/image.height:(y+0.5f)/image.height;
                const float ny=1-2*rawY;
                const float* inv=cb.data()+48;
                const float hz=nx*inv[2]+ny*inv[6]+depth*inv[10]+inv[14];
                const float hw=nx*inv[3]+ny*inv[7]+depth*inv[11]+inv[15];
                const float z=std::abs(hw)>1e-8f?std::abs(hz/hw):100000;
                const float value=std::isfinite(z)?std::clamp(1-std::log2(1+z)/std::log2(100001.0f),0.0f,1.0f):0;
                depthPreview[pos]=depthPreview[pos+1]=depthPreview[pos+2]=uint8_t(value*255);depthPreview[pos+3]=255;
            }
            Png(factory.Get(),folder/(image.name+".png"),image.width,image.height,depthPreview.data());
            preview(image,depthPreview.data());
        } else {
            Png(factory.Get(),folder/(image.name+".png"),image.width,image.height,image.bytes.data());
            preview(image,image.bytes.data());
        }
    }
    // Shareable desktop photos as well as the adjustable local report.
    for(int eye=0;eye<2;++eye) {
        const auto& real=images[eye*5];const auto& predicted=images[eye*5+1];const auto& next=images[eye*5+4];
        for(int mode=0;mode<3;++mode) {
            std::vector<uint8_t> pixels(real.bytes.size());
            const auto& other=mode==1?next:real;
            for(size_t i=0;i<pixels.size();i+=4) {
                for(int c=0;c<3;++c) pixels[i+c]=mode==2?
                    uint8_t(std::min(255,4*std::abs(int(predicted.bytes[i+c])-int(real.bytes[i+c])))):
                    uint8_t((unsigned(predicted.bytes[i+c])+unsigned(other.bytes[i+c]))/2);
                pixels[i+3]=255;
            }
            const std::string filename=(mode==0?"overlay-original-":mode==1?"overlay-next-":"difference-original-")+std::string(eye==0?"left.png":"right.png");
            Png(factory.Get(),folder/filename,real.width,real.height,pixels.data());
        }
    }
    std::ostringstream json;json<<std::setprecision(9);
    // XR nanoseconds are strings, so JavaScript cannot round away precision.
    json<<"{\"realTime\":\""<<meta.realTime<<"\",\"predictedTime\":\""<<meta.predictedTime<<"\",\"nextTime\":\""<<meta.nextTime
        <<"\",\"horizonMs\":"<<double(meta.predictedTime-meta.realTime)*1e-6
        <<",\"nextAfterPredictionMs\":"<<double(meta.nextTime-meta.predictedTime)*1e-6
        <<",\"submissionKnown\":"<<(meta.submissionKnown?"true":"false")<<",\"submissionResult\":"<<meta.submissionResult
        <<",\"note\":\"Pre-runtime images, not compositor-presented frames. Different timestamps; differences are NOT a ground-truth error metric. Capture allocations/copies/readback perturb timing.\",\"eyes\":[";
    auto array=[&json](const auto& values){json<<"[";bool first=true;for(float v:values){if(!first)json<<",";first=false;if(std::isfinite(v))json<<v;else json<<"null";}json<<"]";};
    for(int e=0;e<2;++e) {
        if(e)json<<",";
        json<<"{\"width\":"<<images[e*5].width<<",\"height\":"<<images[e*5].height
            <<",\"depthWidth\":"<<images[e*5+3].width<<",\"depthHeight\":"<<images[e*5+3].height
            <<",\"sourceFlipV\":"<<(meta.flip[e]?"true":"false")
            <<",\"playerMaskActive\":"<<(meta.constants[e][65]>=3?"true":"false")<<",\"warpConstants\":";
        array(meta.constants[e]);json<<",\"cachedPose\":";array(meta.cachedPose[e]);
        json<<",\"targetPose\":";array(meta.targetPose[e]);json<<",\"nextPose\":";array(meta.nextPose[e]);
        json<<",\"fov\":";array(meta.fov[e]);json<<"}";
    }
    json<<"],\"warpAlgorithm\":\""<<(meta.constants[0][65]>=3?"source-depth-player-mask-v3":meta.constants[0][65]>=2?"source-depth-boundary-v2":"legacy-uv-confidence-fade")
        <<"\",\"movementUnits\":\"Skyrim world units per CPU wall-clock second; raw actor displacement, not predictor output. Direction relative to normalized captured game-view axes.\",\"movement\":";
    MovementJson(json,meta.movement);json<<",\"nextMovement\":";MovementJson(json,meta.nextMovement);json<<"}";
    SaveText(folder/"metadata.json",json.str());
    SaveText(folder/"metadata.js","window.captureMetadata="+json.str()+";");
    previews<<"};";SaveText(folder/"preview-images.js",previews.str());
    SaveText(folder/"index.html",s_dapaCaptureViewer);
    if(meta.constants[0][65]>=3 || meta.constants[1][65]>=3)
        SaveText(folder/"PLAYER-MASK.txt","Player mask v1: player-mask-left/right.png show depth-validated protected pixels in white. Raw .f32 files store rasterized player depth, -1 outside coverage, in canonical image orientation. Each eye's playerMaskActive states whether the input was available; an empty PNG means no pixels passed validation. Compare masks against real-left/right.png to verify armor/body coverage. Algorithm 3 retains player pixels at real-frame cadence while correcting world motion; it does not predict animation.\r\n");
    SaveText(folder/"README.txt","DAPA single-shot diagnostic. Open index.html, or overlay-original-left.png/right.png in Photos. real = source real frame; prediction = clean pre-runtime DAPA output; next = following real frame at a later timestamp. PNGs are 8-bit RGBA cache values with no added tint or automatic tone mapping; diagnostic PNG R=confidence, G/B=0.5+4*UVDisplacement (quantized/clipped). For warpAlgorithm source-depth-boundary-v2, displacement is the selected source offset and zero confidence may mean unresolved/background-filled, not unchanged. Legacy displacement is the intended offset before confidence fading. A red rejection view is inactive, not a rejection, if warpConstants[64] is zero. Depth .f32 is little-endian raw device depth in canonical image orientation; depth PNG is logarithmic reconstructed view depth, white=near, black=far. Warp constants: 0..15 transform; 16..17 resolution; 18..19 near/far; 20..23 FOV; 24 depthScale; 25 legacy edgeFade (unused in v2); 26 nearFade; 27 game debug tint; 28..29 depth resolution; 30..31 flip; 32..47 projection; 48..63 inverse projection; 64 valid; 65 algorithm version (2 = source-depth-boundary-v2); 66..67 padding. Poses: quaternion xyzw, position xyz. No runtime ATW, VD encoding or headset compositor is captured. Capture overhead is not normal frame timing. No textures are uploaded.\r\n");
    return folder.string();
}
