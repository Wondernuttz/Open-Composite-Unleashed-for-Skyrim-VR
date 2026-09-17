#include "OpenOVR/Misc/Input/LocomotionHeading.h"
#include "OpenOVR/Misc/Input/LocomotionFrame.h"
#include "OpenOVR/Misc/Input/LocomotionCalibrationGesture.h"
#include <iostream>
#include <limits>
#include <stdexcept>

unsigned checks = 0;
void Check(bool ok, const char* why) { ++checks; if (!ok) throw std::runtime_error(why); }
bool Near(double a, double b) { return std::abs(a-b) < .00001; }
int main() {
    try {
        auto& h = OcuLocomotionHeading::Instance();
        const auto reset = [&] { h.Reset(); h.SetFocused(true); h.Read(100, false); };
        const auto publish = [&](std::int64_t time, double yaw) {
            h.Publish(h.CaptureToken(), 100, true, 0, std::sin(yaw/2), 0, std::cos(yaw/2), time);
            return h.Read(100, false);
        };
        reset(); auto before = publish(1000, .6);
        const auto oldToken = h.CaptureToken();
        Check(h.QueueStageChange(2000, true, 0, std::sin(.4/2), 0, std::cos(.4/2)), "known yaw recenter retained");
        Check(!h.Read(100, false).yaw, "event invalidates previous cached heading");
        h.Publish(oldToken, 100, true, 0, 0, 0, 1, 2100);
        Check(!h.Read(100, false).yaw, "in-flight pre-event sample rejected");
        auto pre = publish(1999, .6);
        Check(pre.yaw && Near(*pre.yaw, .6) && pre.referenceEpoch == before.referenceEpoch, "old-space sample before changeTime unchanged");
        locomotion::Receiver aligned;
        locomotion::Frame walking; walking.epoch=3; walking.flags=locomotion::Connected; walking.vz=-1;
        walking.sequence=1; aligned.Ingest(walking,100,pre.yaw);
        walking.sequence=2; aligned.Ingest(walking,110,pre.yaw);
        Check(aligned.Calibrate(110,*pre.yaw),"initial explicit alignment uses observed STAGE heading");
        auto oldMovement=aligned.Mix({},110,pre.yaw);
        auto post = publish(2000, .2);
        Check(post.yaw && Near(*post.yaw, .6) && post.referenceEpoch == before.referenceEpoch, "exact changeTime adds new-origin yaw with correct sign");
        walking.sequence=3; aligned.Ingest(walking,120,post.yaw);
        auto newMovement=aligned.Mix({},120,post.yaw);
        Check(newMovement.source==locomotion::Source::Locomotion && Near(newMovement.stick.x,oldMovement.stick.x) &&
            Near(newMovement.stick.y,oldMovement.stick.y),"real receiver preserves walking vector across known recenter without recalibrating");
        Check(Near(*publish(1999, -1).yaw, .6), "out-of-order old pose cannot overwrite current reference");
        Check(h.QueueStageChange(3000, true, 0, std::sin(-.8/2), 0, std::cos(-.8/2)), "second yaw recenter queued");
        Check(Near(*publish(3000, 1).yaw, .6), "successive rotations compose in stable heading coordinates");
        h.QueueStageChange(4000, false, 0, 0, 0, 0);
        Check(publish(3999, 1).referenceEpoch == before.referenceEpoch, "unknown change not applied early");
        auto unknown = publish(4000, .9);
        Check(unknown.referenceEpoch != before.referenceEpoch && Near(*unknown.yaw, .9), "unknown change demands explicit alignment at matching pose time");
        reset(); publish(1000, 0);
        Check(!h.QueueStageChange(500, true, 0, 0, 0, 1), "late event fails closed rather than double-rotating");
        const auto lateEpoch = h.Read(100,false).referenceEpoch;
        Check(publish(1100, .2).referenceEpoch != lateEpoch, "late event invalidates calibration on next pose");
        reset();
        Check(!h.QueueStageChange(2000, true, std::sin(.2), 0, 0, std::cos(.2)), "tilted STAGE transform is not approximated as yaw");
        const auto tiltEpoch = publish(1000, 0).referenceEpoch;
        Check(publish(2000, 0).referenceEpoch != tiltEpoch, "tilt requires explicit calibration");
        reset();
        auto overflowEpoch = publish(1000, 0).referenceEpoch;
        for (int n = 0; n < 9; ++n) h.QueueStageChange(2000+n*10, true, 0, 0, 0, 1);
        Check(publish(1999, 0).referenceEpoch == overflowEpoch, "queue overflow does not apply future change early");
        Check(publish(2000, 0).referenceEpoch != overflowEpoch, "bounded queue overflow invalidates safely");
        reset(); auto oldSession = publish(1000, .5); reset();
        Check(publish(10, .1).referenceEpoch != oldSession.referenceEpoch, "session reset clears queued changes and coordinate epoch");

        locomotion::Receiver r; locomotion::Frame f; f.epoch=1; f.flags=locomotion::Connected; f.vz=-1;
        std::uint64_t now=100;
        const auto send = [&](std::optional<double> yaw) {
            ++f.sequence; now+=10;
            Check(r.Ingest(f,now,yaw)==locomotion::Error::None,"fresh sensor frame accepted");
        };
        send({}); send({}); ++f.calibrationSerial; send({});
        Check(r.CalibrationPending(), "explicit calibration waits for current headset pose");
        Check(r.Mix({},now,{},true,false).status==locomotion::Status::NoHmdPose,"pending calibration cannot cause movement without pose");
        send(.4);
        Check(!r.CalibrationPending() && r.CalibrationGeneration()==1,"sensor request recovers on next eligible heading without second button");
        r.Mix({},now,.4); send(.4);
        Check(r.Mix({},now,.4).source==locomotion::Source::Locomotion,"fresh resume sequence restores walking");
        ++f.calibrationSerial; send({}); r.InvalidateReference(); send(.7);
        Check(r.CalibrationGeneration()==2 && r.Mix({},now,.7).source==locomotion::Source::Locomotion,"explicit pending alignment survives matching reference transition");
        r.InvalidateReference(); send(.7);
        Check(r.Mix({},now,.7).status==locomotion::Status::Uncalibrated,"unknown reference alone never auto-aligns body to headset");
        ++f.calibrationSerial; send({});
        const auto generation=r.CalibrationGeneration();
        for(unsigned i=0;i<510;++i) send({});
        send(.7);
        Check(!r.CalibrationPending() && r.CalibrationGeneration()==generation,"expired request cannot calibrate after user has turned away");
        ++f.calibrationSerial; send({}); f.flags=0; send({}); f.flags=locomotion::Connected; send(.7); send(.7);
        Check(r.CalibrationGeneration()==generation && !r.CalibrationPending(),"disconnect cancels pending calibration");
        ++f.calibrationSerial; send({}); now+=251; send(.7);
        Check(r.CalibrationGeneration()==generation && !r.CalibrationPending(),"stale sensor stream cancels pending calibration");
        send(.7); Check(r.Calibrate(now,.7),"deliberate controller request calibrates fresh pair");

        OcuLocomotionCalibrationGesture gesture; std::uint64_t tick=1;
        const auto step=[&](bool available,bool left,bool right,bool centered=true) {
            tick+=100; return gesture.Update(tick,available,left,right,centered);
        };
        for(int i=0;i<25;++i) Check(!step(true,true,true),"held buttons at startup cannot calibrate");
        Check(!step(true,false,false),"release arms deliberate gesture");
        for(int i=0;i<20;++i) Check(!step(true,true,true),"short hold does not calibrate");
        Check(step(true,true,true),"two-second centered hold calibrates once");
        for(int i=0;i<25;++i) Check(!step(true,true,true),"continued hold cannot repeat calibration");
        step(true,false,false); step(true,true,true); step(false,true,true);
        for(int i=0;i<25;++i) Check(!step(true,true,true),"focus/menu loss cancels and requires release");
        step(true,false,false); step(true,true,true); step(true,true,true,false);
        for(int i=0;i<25;++i) Check(!step(true,true,true),"stick deflection cancels without rearming held controls");
        step(true,false,false); step(true,true,true); tick+=1000;
        for(int i=0;i<25;++i) Check(!step(true,true,true),"missing input samples cancel timed hold");
        step(true,false,false); step(true,true,true); step(true,false,true);
        for(int i=0;i<25;++i) Check(!step(true,true,true),"both controls must release after interrupted hold");
        std::cout << checks << " recenter, calibration recovery and gesture checks passed\n";
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
