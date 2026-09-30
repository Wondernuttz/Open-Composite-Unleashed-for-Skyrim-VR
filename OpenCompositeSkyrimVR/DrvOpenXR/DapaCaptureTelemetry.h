#pragma once
#include "DapaMotion.h"
#include <array>
#include <chrono>
#include <mutex>

namespace DapaCaptureTelemetry {
inline int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Input {
    int64_t timeNs=0;
    std::array<float,4> axes{}; // physical left X/Y, right X/Y, before remap/deadzone
    std::array<bool,4> active{};
};
inline std::mutex inputMutex;
inline Input input;
inline void Publish(Input value) { std::lock_guard<std::mutex> lock(inputMutex);input=value; }
inline Input Read() {
    std::unique_lock<std::mutex> lock(inputMutex,std::try_to_lock);
    return lock.owns_lock()?input:Input{}; // never stall the render thread
}
struct Sample {
    int64_t xrTime=0,previousXrTime=0,cpuTimeNs=0,previousCpuTimeNs=0;
    bool positionValid=false,velocityValid=false,discontinuity=false;
    double cpuInterval=0,xrInterval=0;
    DapaMotion::Vec3 position{},previousPosition{},delta{},velocityCpu{},velocityXr{},predictorVelocity{};
    float speed=0,predictorConfidence=0;
    float actorYaw=0,turnRate=0,turnConfidence=0;
    bool turnInput=false,turnValid=false;
    bool Stationary() const { return velocityValid && speed<0.1f; }
    std::array<DapaMotion::Vec3,2> eyeVelocity{}; // right/up/forward, world units/sec; normalized view axes
    std::array<bool,2> eyeValid{};
    std::array<std::array<float,16>,2> views{};
    Input sticks{};
};
struct Measurement {
    Sample latest{};
    void Reset() { latest={}; }
    void SamplePosition(int64_t xr,int64_t cpu,DapaMotion::Vec3 p) {
        const auto prev=latest;latest={};latest.xrTime=xr;latest.cpuTimeNs=cpu;
        latest.positionValid=xr>0 && cpu>0 && DapaMotion::Finite(p);
        if(!latest.positionValid)return;
        latest.position=p;
        if(!prev.positionValid)return;
        latest.previousPosition=prev.position;latest.previousXrTime=prev.xrTime;latest.previousCpuTimeNs=prev.cpuTimeNs;
        latest.delta=p-prev.position;
        latest.cpuInterval=double(cpu-prev.cpuTimeNs)*1e-9;
        latest.xrInterval=double(xr-prev.xrTime)*1e-9;
        latest.discontinuity=DapaMotion::Length(latest.delta)>15.0f;
        if(latest.cpuInterval>0)latest.velocityCpu=latest.delta*float(1/latest.cpuInterval);
        if(latest.xrInterval>0)latest.velocityXr=latest.delta*float(1/latest.xrInterval);
        latest.speed=DapaMotion::Length(latest.velocityCpu);
        latest.velocityValid=latest.cpuInterval>=0.001 && latest.cpuInterval<=0.250 && latest.xrInterval>0
            && !latest.discontinuity && DapaMotion::Finite(latest.velocityCpu);
    }
    void SetView(int eye,const float* view,float forwardSign) {
        if(eye<0||eye>1||!view)return;
        std::copy(view,view+16,latest.views[eye].begin());
        auto v=DapaMotion::ToView(latest.velocityCpu,view);
        float lengths[3];
        for(int c=0;c<3;++c)lengths[c]=std::sqrt(view[c]*view[c]+view[4+c]*view[4+c]+view[8+c]*view[8+c]);
        if(!latest.velocityValid || !DapaMotion::Finite(v) || !std::isfinite(forwardSign))return;
        for(float length:lengths)if(!std::isfinite(length)||length<1e-6f)return;
        latest.eyeVelocity[eye]={v.x/lengths[0],v.y/lengths[1],v.z/lengths[2]*forwardSign};latest.eyeValid[eye]=true;
    }
};
}
