#include "stdafx.h"
#include "PointerState.h"
#include "LaserCalibration.h"
#include "Keyboard/LaserRaySmoothing.h"
#include "Keyboard/LaserPointerStyle.h"
#include "Reimpl/BaseInput.h"
#include "Reimpl/BaseSystem.h"
#include "generated/static_bases.gen.h"
#include <atomic>

namespace ocu_pointer {
namespace { std::atomic<std::int64_t> pendingReferenceChange{ 0 }; }
void ResetPointerRuntime()
{
    pendingReferenceChange.store(0);
    GetPointerState().Reset();
}
void SchedulePointerReferenceChange(std::int64_t time)
{
    auto pending = pendingReferenceChange.load();
    while (pending < time && !pendingReferenceChange.compare_exchange_weak(pending, time)) {}
    GetPointerState().Reset();
}
void PublishPointerFrame()
{
    const auto pending = pendingReferenceChange.load();
    if (pending) {
        if (!xr_gbl || xr_gbl->nextPredictedFrameTime < pending) return;
        pendingReferenceChange.store(0);
        GetPointerState().Reset();
    }
    const auto now = PointerClockTicks();
    const auto interest = GetPointerState().ReadInterest(now);
    if (!interest.active) return;
    auto* sys = GetUnsafeBaseSystem();
    auto input = GetBaseInput();
    if (!sys || !xr_gbl || !xr_session.get()) { GetPointerState().Reset(); return; }
    const XrSpace space = xr_space_from_ref_space_type(sys->currentSpace);
    Snapshot value{};
    value.publicationQpc = now;
    value.qpcFrequency = PointerClockFrequency();
    value.predictedDisplayTime = xr_gbl->nextPredictedFrameTime;
    value.appliedPitchTrimDegrees = interest.pitch;
    for (int state = 0; state < 2; ++state) {
        for (int channel = 0; channel < 4; ++channel)
            value.style.beamColor[state][channel] = oovr_laser_style::Beam[state][channel] / 255.0f;
        for (int channel = 0; channel < 3; ++channel)
            value.style.dotColor[state][channel] = oovr_laser_style::DotCore[state][channel] / 255.0f;
        value.style.dotColor[state][3] = oovr_laser_style::DotCoreAlpha;
    }
    value.trackingOrigin = sys->currentSpace == XR_REFERENCE_SPACE_TYPE_STAGE ? 1u : 0u;
    static bool trigger[2] = {};
    static float lastPitch = 1000.0f;
    static std::uint64_t lastGeneration = 0;
    if (interest.generation != lastGeneration) {
        trigger[0] = trigger[1] = false;
        lastGeneration = interest.generation;
        lastPitch = 1000.0f;
    }
    if (interest.pitch != lastPitch) {
        for (int side = 0; side < 2; ++side)
            oovr_laser_smoothing::Reset(oovr_laser_smoothing::Consumer::External, side, space);
        lastPitch = interest.pitch;
    }
    for (int side = 0; side < 2; ++side) {
        auto& hand = value.hands[side];
        bool digital = false;
        float analog = 0.0f;
        if (sys->IsInputAvailable() && input && input->SamplePhysicalTrigger(side + 1, digital, analog)) {
            trigger[side] = digital || analog >= (trigger[side] ? 0.3f : 0.7f);
            hand.flags = InputAvailable | (trigger[side] ? TriggerDown : 0u);
        } else {
            trigger[side] = false;
        }
        XrSpace aim = XR_NULL_HANDLE;
        if (input && input->AreActionsLoaded()) input->GetHandSpace(side + 1, aim, true);
        XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
        if (!aim || XR_FAILED(xrLocateSpace(aim, space, value.predictedDisplayTime, &location)) ||
            (location.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) !=
                (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            oovr_laser_smoothing::Reset(oovr_laser_smoothing::Consumer::External, side, space);
            continue;
        }
        XrVector3f origin = location.pose.position, down, direction;
        rotate_vector_by_quaternion({ 0, -1, 0 }, location.pose.orientation, down);
        const float offset = oovr_laser_calibration::OriginDown(side);
        origin.x += down.x * offset; origin.y += down.y * offset; origin.z += down.z * offset;
        const auto forward = oovr_laser_calibration::RotateLocalVector(
            oovr_laser_calibration::LocalForward(side), interest.pitch, 0, 0);
        rotate_vector_by_quaternion(forward, location.pose.orientation, direction);
        if (!oovr_laser_smoothing::Filter(oovr_laser_smoothing::Consumer::External, side,
            space, value.predictedDisplayTime, origin, direction)) continue;
        hand.origin[0] = origin.x; hand.origin[1] = origin.y; hand.origin[2] = origin.z;
        hand.direction[0] = direction.x; hand.direction[1] = direction.y; hand.direction[2] = direction.z;
        hand.flags |= RayValid;
    }
    GetPointerState().Publish(reinterpret_cast<std::uintptr_t>(xr_session.get()),
        reinterpret_cast<std::uintptr_t>(space), value);
    lastGeneration = GetPointerState().ReadInterest(now).generation;
}
}
