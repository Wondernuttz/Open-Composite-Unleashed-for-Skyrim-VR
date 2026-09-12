// Extracted production sampler, production projection, and fake OpenXR results.
// No eye device, action syncing, SteamVR, or game initialization is required.
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include "OpenOVR/Misc/EyeGaze.h"
#include "OpenOVR/Compositor/VRSGaze.h"
#define OOVR_LOGF(...) ((void)0)
#define OOVR_LOG_LIMITEDF(...) ((void)0)
#define OOVR_FAILED_XR_SOFT_ABORT(expr) ((void)(expr))
bool oovr_debug_logging_enabled() { return false; }
template<class T> T Handle(unsigned value) { return reinterpret_cast<T>(static_cast<uintptr_t>(value)); }
const XrPosef identity{{0,0,0,1},{0,0,0}};
struct FakeGlobals {
    XrSpace viewSpace=Handle<XrSpace>(1);
    bool viewSpaceViewsLatched=false;
    XrViewStateFlags latchedViewSpaceFlags=0;
    XrView latchedViewSpaceViews[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
} globals;
FakeGlobals* xr_gbl=&globals;
struct FakeSession {
    XrSession value=Handle<XrSession>(2);
    XrSession lock_shared() const { return value; }
} xr_session;
std::mutex xr_session_call_mutex;
struct FakeRuntime {
    XrResult stateResult=XR_SUCCESS,locateResult=XR_SUCCESS,viewsResult=XR_SUCCESS;
    XrBool32 active=XR_TRUE;
    XrSpaceLocationFlags flags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    XrQuaternionf orientation{0,0,0,1};
    XrTime time=42;
    XrViewStateFlags viewFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    uint32_t viewCount=2;
    XrPosef eyePoses[2]{identity,identity};
    unsigned states=0,locations=0,viewLocations=0,destroyed=0;
} runtime;
XrResult xrGetActionStatePose(XrSession,const XrActionStateGetInfo*,XrActionStatePose* state) {
    ++runtime.states; state->isActive=runtime.active; return runtime.stateResult;
}
XrResult xrLocateSpace(XrSpace,XrSpace,XrTime,XrSpaceLocation* location) {
    ++runtime.locations; location->locationFlags=runtime.flags; location->pose.orientation=runtime.orientation;
    static_cast<XrEyeGazeSampleTimeEXT*>(location->next)->time=runtime.time; return runtime.locateResult;
}
XrResult xrLocateViews(XrSession,const XrViewLocateInfo*,XrViewState* state,uint32_t,uint32_t* count,XrView* views) {
    ++runtime.viewLocations; state->viewStateFlags=runtime.viewFlags; *count=runtime.viewCount;
    views[0].pose=runtime.eyePoses[0]; views[1].pose=runtime.eyePoses[1]; return runtime.viewsResult;
}
XrResult xrDestroySpace(XrSpace) { ++runtime.destroyed; return XR_SUCCESS; }
class BaseInput {
public:
    XrAction eyeGazeAction=Handle<XrAction>(3);
    XrSpace eyeGazeSpace=Handle<XrSpace>(4);
    XrPosef eyeGazeViewPoses[2]{identity,identity};
    bool eyeGazeViewPosesValid=false,eyeGazeLiveReported=false;
    void DestroyEyeGazeSpace();
    bool SampleEyeGazeDirection(XrTime,XrVector3f&,XrPosef[2],XrTime&);
};
#include "EyeGazeSamplerProduction.inc"
static unsigned checks=0;
void Check(bool value,const char* reason) { ++checks; if(!value) throw std::runtime_error(reason); }
bool Close(float a,float b) { return std::fabs(a-b)<.00001f; }
XrQuaternionf Yaw(float radians) { return {0,std::sin(radians*.5f),0,std::cos(radians*.5f)}; }
int main() {
 try {
    BaseInput input; XrVector3f direction{}; XrPosef poses[2]{}; XrTime sampleTime=-1;
    const XrTime display=10'000'000'000;
    auto sample=[&]() { return input.SampleEyeGazeDirection(display,direction,poses,sampleTime); };
    Check(sample(),"tracked valid gaze accepted");
    Check(runtime.viewLocations==1 && Close(direction.z,-1),"initial calibration and ray");
    const XrTime times[]{0,42,display-5'000'000'000,display,display+5'000'000'000};
    // Runtime sample times are metadata; fresh poses must move even if times do not.
    for(auto time:times) {
        runtime.time=time;
        for(float yaw:{-.3f,.0f,.3f}) {
            runtime.orientation=Yaw(yaw); Check(sample(),"permitted runtime timestamp accepted");
            Check(sampleTime==time && Close(direction.x,-std::sin(yaw)),"new pose is sampled independent of timestamp");
        }
    }
    Check(runtime.viewLocations==1,"eye calibration cached, gaze ray not cached");
    // Characterize current VALID-only acceptance; this is not a new tracked-bit gate.
    runtime.flags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    runtime.orientation=Yaw(.15f);
    const auto calls=runtime.locations;
    for(unsigned frame=0;frame<100;++frame) Check(sample(),"valid untracked pose remains accepted");
    Check(runtime.locations==calls+100,"unchanging valid pose is queried each call");
    auto rejectAndRecover=[&](const char* reason) {
        Check(!sample(),reason);
        runtime.stateResult=runtime.locateResult=XR_SUCCESS; runtime.active=XR_TRUE;
        runtime.flags=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        runtime.orientation=Yaw(-.2f);
        Check(sample() && Close(direction.x,std::sin(.2f)),"fresh movement returns immediately after loss");
    };
    runtime.active=XR_FALSE; rejectAndRecover("inactive gaze rejected");
    runtime.stateResult=XR_ERROR_SESSION_LOST; rejectAndRecover("state error rejected");
    runtime.locateResult=XR_ERROR_TIME_INVALID; rejectAndRecover("locate error rejected");
    runtime.flags=0; rejectAndRecover("invalid orientation rejected");
    runtime.orientation={0,0,0,0}; rejectAndRecover("degenerate orientation rejected");
    runtime.orientation={0,1,0,0}; rejectAndRecover("backward ray rejected");
    runtime.orientation={0,0,0,std::numeric_limits<float>::quiet_NaN()}; rejectAndRecover("non-finite ray rejected");
    // Published eye calibration can change, including canted display poses.
    globals.viewSpaceViewsLatched=true; globals.latchedViewSpaceFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    globals.latchedViewSpaceViews[0].pose=identity; globals.latchedViewSpaceViews[0].pose.orientation=Yaw(-.15f);
    globals.latchedViewSpaceViews[1].pose=identity; globals.latchedViewSpaceViews[1].pose.orientation=Yaw(.15f);
    runtime.orientation=Yaw(.1f); Check(sample(),"fresh latched eye poses accepted");
    Check(Close(poses[0].orientation.y,std::sin(-.075f)) && Close(poses[1].orientation.y,std::sin(.075f)),"new cant reaches projection");
    ocu_vrs_gaze::Center centers[2];
    for(unsigned eye=0;eye<2;++eye) {
        const auto q=poses[eye].orientation;
        Check(ocu_vrs_gaze::ProjectViewSpace(direction.x,direction.y,direction.z,q.x,q.y,q.z,q.w,
            -1,1,1,-1,centers[eye]),"per-eye ray projects");
    }
    Check(std::fabs(centers[0].x-centers[1].x)>.1f,"canted eyes retain distinct centers");
    input.DestroyEyeGazeSpace(); Check(!input.eyeGazeViewPosesValid && !input.eyeGazeLiveReported,"session destruction clears cached eye calibration and live marker");
    Check(!sample(),"destroyed space cannot keep previous gaze alive");
    input.eyeGazeSpace=Handle<XrSpace>(5); globals.viewSpaceViewsLatched=false;
    runtime.eyePoses[0].orientation=Yaw(-.05f); runtime.eyePoses[1].orientation=Yaw(.05f);
    Check(sample() && runtime.viewLocations==2,"new session reacquires calibration");
    xr_session.value=XR_NULL_HANDLE; Check(!sample() && sampleTime==0,"missing session rejects and resets timestamp");
    xr_session.value=Handle<XrSession>(6);
    input.DestroyEyeGazeSpace(); input.eyeGazeSpace=Handle<XrSpace>(7);
    runtime.viewsResult=XR_ERROR_RUNTIME_FAILURE; Check(!sample(),"missing initial per-eye calibration rejects");
    runtime.viewsResult=XR_SUCCESS; Check(sample(),"per-eye calibration failure retries immediately");
    Check(!input.SampleEyeGazeDirection(0,direction,poses,sampleTime),"invalid display time rejected");
    std::printf("PASS: %u production gaze sampler checks; no validity/timestamp policy changed\n",checks); return 0;
 } catch(const std::exception& error) { std::printf("FAIL: %s\n",error.what()); return 1; }
}
