// Runs the extracted production synthetic-frame scheduler and stereo lifecycle
// against fake OpenXR/D3D endpoints. No runtime or GPU is initialized.
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <vector>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include "DrvOpenXR/DapaTiming.h"
#include "OpenOVR/Misc/FoveationBlackout.h"
#define OOVR_LOGF(...) ((void)0)
template<class T> T Handle(unsigned n) { return reinterpret_cast<T>(static_cast<uintptr_t>(n)); }
static unsigned checks=0;
void Check(bool value,const char* reason) { ++checks; if(!value) throw std::runtime_error(reason); }
struct Bridge { unsigned char isMenuOpen=0,isMainMenu=0,isLoadingScreen=0; } bridge;
Bridge* s_pBridge=&bridge;
static unsigned bridgeReads=0,openOnRead=0;
static bool maskCacheValid=true;
static unsigned maskReads=0,invalidateMaskOnRead=0;
bool OCBridge_DapaMaskCacheValid() {
    if(++maskReads==invalidateMaskOnRead)maskCacheValid=false;
    return maskCacheValid;
}
void OpenRenderTargetBridge() { if(++bridgeReads==openOnRead) bridge.isMenuOpen=1; }
#include "DapaMenuBridge.inc"
enum class Phase { None, Wait, Begin, Locate, Warp, Copy, End };
static Phase openAt=Phase::None;
void Enter(Phase phase) { if(phase==openAt) bridge.isMenuOpen=1; }
struct ID3D11DeviceContext { unsigned releases=0; void Release() {++releases;} } context;
struct Device { void GetImmediateContext(ID3D11DeviceContext** out) { *out=&context; } } device;
struct ResetState { unsigned resets=0; void Reset() {++resets;} void HistoryInvalidated() {++resets;} };
class ASWProvider {
public:
    bool m_ready=true,m_paused=false,m_hasCachedFrame=false,m_injectionWanted=true;
    uint8_t m_cacheBuildEyeMask=0;
    bool m_motionGeometryValid[2]{};
    ocu_foveation::BlackoutFrame m_cachedBlackout[2]{};
    ResetState m_capture,m_motion,m_captureMovement,m_turn;
    unsigned warps=0,copies=0;
#include "DapaMenuProvider.inc"
    bool IsReady() const {return m_ready;}
    bool HasCachedFrame() const {return m_hasCachedFrame;}
    bool IsPaused() const {return m_paused;}
    bool IsInjectionWanted() const {return m_injectionWanted;}
    Device* GetDevice() {return &device;}
    void SetWarpDisplayTime(XrTime) {}
    bool WarpFrame(int,ID3D11DeviceContext*,XrPosef) {Enter(Phase::Warp);++warps;return !m_paused&&m_hasCachedFrame;}
    bool SubmitWarpedOutput(ID3D11DeviceContext*,double) {Enter(Phase::Copy);++copies;return !m_paused&&m_hasCachedFrame;}
    XrSwapchain GetDepthSwapchain() {return XR_NULL_HANDLE;}
    bool HasSubmittedDepth() {return false;}
    XrPosef GetCachedPose(int) {return {{0,0,0,1},{0,0,0}};}
    XrFovf GetCachedFov(int) {return {-.7f,.7f,.7f,-.7f};}
    XrSwapchain GetOutputSwapchain() {return Handle<XrSwapchain>(1);}
    XrRect2Di GetOutputRect(int eye) {return {{eye*100,0},{100,100}};}
    float GetCachedNear() {return .1f;}
    float GetCachedFar() {return 1000;}
    void CaptureSubmission(XrTime,XrResult) {}
    bool CaptureBusy() {return false;}
    bool CaptureRecording() {return false;}
} provider;
ASWProvider* g_aswProvider=&provider;
bool bridgeResourcesReady=true;
struct BridgeResources {void* mvTexture=reinterpret_cast<void*>(1);} bridgeResources;
bool ValidateBridgeTexture(void*,const char*) {return true;}
#include "DapaMenuCacheAdmission.inc"
struct Session {XrSession get(){return Handle<XrSession>(1);} int lock_shared(){return 0;}} xr_session;
struct System {int currentSpace=0;} fakeSystem;
System* GetUnsafeBaseSystem(){return &fakeSystem;}
XrSpace xr_space_from_ref_space_type(int){return Handle<XrSpace>(1);}
struct Overlay {bool PositionOverScene(const XrCompositionLayerProjection&){return true;}};
Overlay* foveationDebugOverlay=nullptr;
const XrCompositionLayerBaseHeader* debugHeaders[2]{};
XrCompositionLayerProjection scene{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
const XrCompositionLayerBaseHeader* headers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&scene)};
int layer_count=1;
bool app_layer=true,prepareInjection=true;
XrResult realEndResult=XR_SUCCESS;
struct Guard {double backoffMs=0; unsigned clean=0; void CleanInjection(double){++clean;}} recovery,pacing;
DapaTiming::PeriodBaseline dapaPeriodBaseline;
double dapaTimingPeriodMs=1000./90.,predictedDisplayPeriodMs=1000./90.,recoveryElapsedMs=1000./90.;
float s_aswLastWarpWaitMs=0,measuredEndFrameMs=0;
constexpr int s_aswInjectCount=1,XruEyeCount=2;
auto realEndDone=DapaTiming::Clock::now();
XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
struct Stats {unsigned held=0,attempts=0,syntheticHidden=0,empty=0,synthetic=0;double waitMs=0,endMs=0,maxEndMs=0;} dapaStats;
struct Config {bool DebugLogging(){return false;}} oovr_global_configuration;
static unsigned trouble=0,pacingObserved=0,waits=0,begins=0,ends=0,emptyEnds=0,renderedEnds=0;
void aswTrouble(const char*,double){++trouble;}
void observePacing(double,double){++pacingObserved;}
XrResult xrWaitFrame(XrSession,const XrFrameWaitInfo*,XrFrameState* state) {
    Enter(Phase::Wait);++waits;state->shouldRender=XR_TRUE;state->predictedDisplayTime=1'000'000'000;
    state->predictedDisplayPeriod=11'111'111;return XR_SUCCESS;
}
XrResult xrBeginFrame(XrSession,const XrFrameBeginInfo*){Enter(Phase::Begin);++begins;return XR_SUCCESS;}
XrResult xrLocateViews(XrSession,const XrViewLocateInfo*,XrViewState* state,uint32_t,uint32_t* count,XrView* views){
    Enter(Phase::Locate);*count=2;state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
    views[0].pose=views[1].pose={{0,0,0,1},{0,0,0}};return XR_SUCCESS;
}
XrResult xrEndFrame(XrSession,const XrFrameEndInfo* frame){
    Enter(Phase::End);++ends;if(frame->layerCount==0)++emptyEnds;else ++renderedEnds;return XR_SUCCESS;
}
#include "DapaMenuScheduler.inc"
void Reset() {
    provider=ASWProvider{};bridge={};s_pBridge=&bridge;bridgeReads=openOnRead=0;openAt=Phase::None;
    maskCacheValid=true;maskReads=invalidateMaskOnRead=0;
    trouble=pacingObserved=waits=begins=ends=emptyEnds=renderedEnds=0;context={};dapaStats={};
    recovery={};pacing={};prepareInjection=true;
}
void Pair() {Check(CanCache(),"cache admitted for gameplay");Check(provider.CacheEye(0),"fresh left eye");
    Check(!provider.HasCachedFrame(),"partial pair cannot inject");Check(provider.CacheEye(1),"fresh right eye");
    Check(provider.HasCachedFrame(),"complete fresh pair published");}
int main() {
 try {
    Reset();Pair();RunScheduler();Check(renderedEnds==1&&provider.warps==2,"ordinary gameplay injects once");
    for(unsigned state=1;state<8;++state){
        Reset();Pair();bridge.isMenuOpen=state&1;bridge.isMainMenu=(state>>1)&1;bridge.isLoadingScreen=(state>>2)&1;
        RunScheduler();Check(waits==0&&provider.IsPaused(),"every menu combination prevents slot claim");
        Check(!provider.HasCachedFrame()&&!provider.IsInjectionWanted()&&!CanCache(),"menu invalidates pair and blocks caching");
        Check(!provider.CacheEye(0),"provider refuses paused cache even if caller bypassed");
        const auto resets=provider.m_motion.resets;
        for(unsigned frame=0;frame<100;++frame)RunScheduler();
        Check(waits==0&&provider.m_motion.resets==resets,"persistent menu remains suspended without repeated resets");
        Check(!provider.WarpFrame(0,&context,{})&&!provider.SubmitWarpedOutput(&context,11),"paused provider refuses warp and output");
        bridge={};RunScheduler();Check(!provider.IsPaused()&&!provider.HasCachedFrame()&&waits==0,"closing does not restore old pair");
        Check(!provider.CacheEye(1),"right eye alone cannot reuse pre-menu left");
        Check(provider.CacheEye(0),"new left accepted on resume");RunScheduler();Check(waits==0,"half pair remains uninjected");
        Check(provider.CacheEye(1),"new right completes resumed pair");RunScheduler();Check(renderedEnds==1,"fresh stereo resumes injection");
    }
    Reset();Pair();bridge.isMenuOpen=1;RunScheduler();
    // SKSE owns nested-menu counting: its flag stays true until the last closes.
    RunScheduler();Check(waits==0,"closing one of nested menus leaves published pause active");
    bridge.isMenuOpen=0;RunScheduler();Pair();RunScheduler();Check(renderedEnds==1,"last nested close resumes only after fresh pair");
    Reset();Check(provider.CacheEye(0),"partial pre-menu left");bridge.isMenuOpen=1;RunScheduler();
    bridge.isMenuOpen=0;RunScheduler();Check(!provider.CacheEye(1),"menu between real eyes invalidates generation");
    for(unsigned boundary:{1u,2u}) {
        Reset();Pair();openOnRead=boundary;RunScheduler();
        Check(waits==0&&ends==0&&!provider.HasCachedFrame(),"menu before claim creates no synthetic XR frame");
    }
    for(auto phase:{Phase::Wait,Phase::Begin,Phase::Locate,Phase::Warp,Phase::Copy,Phase::End}){
        Reset();Pair();openAt=phase;RunScheduler();
        Check(waits==1&&begins==1&&ends==1&&renderedEnds==1&&emptyEnds==0,"claimed slot finishes normally without black empty layers");
        Check(context.releases==1&&trouble==0,"in-flight completion preserves cleanup and error policy");
        const auto warped=provider.warps;
        RunScheduler();Check(waits==1&&provider.warps==warped&&!provider.HasCachedFrame(),"next boundary stops persistent menu warping");
    }
    Reset();Pair();pacing.backoffMs=10;RunScheduler();
    Check(waits==0&&provider.HasCachedFrame()&&provider.IsInjectionWanted(),"existing pacing yield preserves cache history");
    Reset();Pair();invalidateMaskOnRead=1;RunScheduler();
    Check(maskReads==1 && waits==0 && begins==0 && ends==0 && dapaStats.attempts==0,
        "late mask conflict is checked before claiming any synthetic XR slot");
    Check(!provider.HasCachedFrame() && provider.m_cacheBuildEyeMask==0 && provider.warps==0 && provider.copies==0,
        "late mask conflict invalidates complete stereo cache before warp or copy");
    Check(context.releases==1 && trouble==0,"mask rejection releases context without creating runtime error/backoff");
    maskCacheValid=true;invalidateMaskOnRead=0;RunScheduler();
    Check(waits==0 && !provider.HasCachedFrame(),"clean mask state alone cannot restore invalidated stereo pair");
    Check(!provider.CacheEye(1),"new right eye cannot reuse left from mask-conflicted pair");
    Check(provider.CacheEye(0),"new left begins recovery after mask conflict");RunScheduler();
    Check(waits==0,"partial recovery pair cannot claim a slot");
    Check(provider.CacheEye(1),"new right completes recovery after mask conflict");RunScheduler();
    Check(waits==1 && renderedEnds==1 && emptyEnds==0 && provider.warps==2,
        "fresh pair and valid mask resume normal synthetic submission");
    std::printf("PASS: %u production menu/cache/scheduler checks; max one already-claimed slot, no menu-empty frame\n",checks);return 0;
 }catch(const std::exception& error){std::printf("FAIL: %s\n",error.what());return 1;}
}
