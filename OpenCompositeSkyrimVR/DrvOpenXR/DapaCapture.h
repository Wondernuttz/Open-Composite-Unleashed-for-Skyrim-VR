#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <string>
#include <vector>
#include "DapaCaptureTelemetry.h"

// Render-thread ownership of D3D objects. Worker receives CPU bytes only.
class DapaCapture {
public:
    explicit DapaCapture(std::filesystem::path output = {}) : root(std::move(output)) {}
    struct Metadata {
        int64_t realTime=0, predictedTime=0, nextTime=0;
        int submissionResult=0;
        bool submissionKnown=false;
        std::array<std::array<float,68>,2> constants{};
        std::array<bool,2> flip{};
        std::array<std::array<float,7>,2> cachedPose{}, targetPose{}, nextPose{};
        std::array<std::array<float,4>,2> fov{};
        DapaCaptureTelemetry::Sample movement{},nextMovement{};
    };
    ~DapaCapture();
    void Poll();
    void ObserveMovement(const DapaCaptureTelemetry::Sample& sample);
    void Request(unsigned delayMs=3000);
    void ToggleSession();
    void StopSession();
    bool Recording() const { return recording; }
    bool ShaderReady() const { return !shaderCode.empty(); }
    void Abort(const char* reason);
    ID3D11ComputeShader* BeginEye(int eye,ID3D11DeviceContext* ctx,
        ID3D11Texture2D* real,ID3D11Texture2D* depth,const float* constants,
        int64_t realTime,int64_t predictedTime,bool flip,
        const float* cachedPose,const float* targetPose,const float* fov,ID3D11Texture2D* bodyMask=nullptr);
    ID3D11UnorderedAccessView* Clean(int eye) { return cleanUav[eye].Get(); }
    ID3D11UnorderedAccessView* Diagnostic(int eye) { return diagnosticUav[eye].Get(); }
    void EndEye(int eye,ID3D11DeviceContext* ctx);
    void Submission(int64_t time,int result);
    void NextPair(ID3D11DeviceContext* ctx,ID3D11Texture2D* const* color,int64_t time,
        const bool* flips,const float* leftPose,const float* rightPose);
    void Pump(ID3D11DeviceContext* ctx);
    bool NeedsPump() const { return state==State::Reading; }
    bool NeedsNext() const { return state==State::Next; }
    void HistoryInvalidated() { if(state==State::Warping||state==State::Next)Abort("real-frame history invalidated"); }
    bool Busy() const { return state!=State::Idle; }
    const std::filesystem::path& OutputRoot() const { return root; }
    static constexpr uint64_t MaxBytes=512ull*1024*1024;
    static uint64_t EstimateBytes(uint32_t w,uint32_t h,uint32_t dw,uint32_t dh) {
        return uint64_t(w)*h*48 + uint64_t(dw)*dh*8;
    }
private:
    enum class State {Idle,Armed,Warping,Next,Reading,Writing};
    struct Image {
        std::string name;
        uint32_t width=0,height=0;
        bool depth=false,flip=false,queued=false,ready=false,bodyMask=false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        std::vector<uint8_t> bytes;
    };
    bool InitializePath();
    void FinishSession();
    void ReleaseGpu();
    void Queue(ID3D11DeviceContext*,ID3D11Texture2D*,Image&);
    void Allocate(ID3D11Device*,ID3D11Texture2D*,Image&);
    static std::string Write(std::filesystem::path,std::vector<Image>,Metadata);
    std::filesystem::path root;
    std::filesystem::path sessionRoot;
    std::vector<std::string> sessionSamples;
    std::vector<DapaCaptureTelemetry::Sample> movementTimeline;
    DapaCaptureTelemetry::Sample latestMovement{};
    unsigned droppedMovementSamples=0;
    bool recording=false;
    unsigned sessionAttempts=0;
    unsigned sessionFailures=0;
    bool photoBudgetReported=false;
    std::chrono::steady_clock::time_point sessionStarted{},nextSample{};
    State state=State::Idle;
    std::chrono::steady_clock::time_point armedAt{},started{},lastPoll{},lastFilePoll{};
    bool keyDown=false;
    uint8_t eyeMask=0;
    uint64_t bytesAllocated=0;
    size_t readingIndex=0;
    Metadata metadata;
    std::vector<Image> images;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Microsoft::WRL::ComPtr<ID3D11Device> shaderDevice;
    std::vector<uint8_t> shaderCode;
    std::future<std::vector<uint8_t>> compiler;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,2> clean,diagnostic;
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>,2> cleanUav,diagnosticUav;
    std::future<std::string> writer;
};
