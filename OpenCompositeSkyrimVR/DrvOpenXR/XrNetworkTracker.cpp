#include "XrNetworkTracker.h"

#include "../OpenOVR/Misc/NetworkTrackers.h"
#include "../OpenOVR/Misc/CameraLegCalibration.h"
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/convert.h"
#include "generated/static_bases.gen.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdio>

namespace {

bool GetAlignedCameraWaist(NetworkTrackerReceiver& receiver,
    const NetTrackerFrameSample& currentFrame, float scale, const glm::quat& alignment,
    const glm::vec3& offset, glm::vec3& waistPosition)
{
	// Use the waist from the same atomic camera frame as the tracker currently
	// being returned. Physical HTCX poses return before this helper is reached.
	NetTrackerSample waist;
	NetTrackerFrameSample waistFrame;
	if (!receiver.GetTracker(0, waist, &waistFrame))
		return false;
	if (currentFrame.everSeen
	    && (!waistFrame.everSeen
	        || waistFrame.sourceEpoch != currentFrame.sourceEpoch
	        || waistFrame.generation != currentFrame.generation))
		return false;

	waistPosition = glm::vec3(waist.pos[0], waist.pos[1], -waist.pos[2]);
	waistPosition = alignment * (waistPosition * scale) + offset;
	return true;
}

bool GetLiveCameraRootCorrection(const glm::vec3& waistPosition,
    glm::vec3& correction)
{
	correction = glm::vec3(0.0f);
	if (!CameraLegCalibration::CapturesInput())
		return false;

	XrSpaceLocation headLocation{ XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
	        xr_gbl->GetBestTime(), &headLocation))
	    || !(headLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return false;

	correction.x = headLocation.pose.position.x - waistPosition.x;
	correction.z = headLocation.pose.position.z - waistPosition.z;

	// A matching camera frame should already be close to its HMD anchor. Reject
	// a nonsensical source jump rather than teleport the entire lower body.
	constexpr float maxCorrection = 0.75f;
	if (!std::isfinite(correction.x) || !std::isfinite(correction.z)
	    || std::abs(correction.x) > maxCorrection
	    || std::abs(correction.z) > maxCorrection) {
		correction = glm::vec3(0.0f);
		return false;
	}
	return true;
}

} // namespace

XrNetworkTracker::XrNetworkTracker(int trackerIdx, vr::TrackedDeviceIndex_t deviceIndex,
    ITrackedDevice* htcxRoleSource)
    : trackerIdx(trackerIdx)
    , htcxRoleSource(htcxRoleSource)
{
	snprintf(serial, sizeof(serial), "OCU-NET%d", trackerIdx + 1);
	InitialiseDevice(deviceIndex);
}

void XrNetworkTracker::GetPose(vr::ETrackingUniverseOrigin origin, vr::TrackedDevicePose_t* pose,
    ETrackingStateType trackingState)
{
	GetPoseImpl(origin, pose, trackingState, true);
}

void XrNetworkTracker::GetPoseForLocomotion(vr::ETrackingUniverseOrigin origin,
    vr::TrackedDevicePose_t* pose, ETrackingStateType trackingState)
{
	GetPoseImpl(origin, pose, trackingState, false);
}

void XrNetworkTracker::GetPoseImpl(vr::ETrackingUniverseOrigin origin, vr::TrackedDevicePose_t* pose,
    ETrackingStateType trackingState, bool allowLocomotionRelease)
{
	// Invalid until fresh data exists; FBT-style consumers ignore invalid
	// poses, so idle slots are harmless.
	ZeroMemory(pose, sizeof(*pose));
	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = false;
	pose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;

	// Canonical waist/foot roles automatically take a live HTCX pose first.
	// A complete runtime role pose takes priority over camera reconstruction;
	// when it disappears, the same OCU-NET device falls back
	// to OSC without making Skyrim-FBT recalibrate or change serial pins.
	if (ITrackedDevice* source = htcxRoleSource.load()) {
		vr::TrackedDevicePose_t htcxPose{};
		source->GetPose(origin, &htcxPose, trackingState);
		if (htcxPose.bPoseIsValid) {
			*pose = htcxPose;
			if (!loggedHtcxSource) {
				OOVR_LOGF("Tracker mux: %s is using its live HTCX/Vive role pose", serial);
				loggedHtcxSource = true;
			}
			return;
		}
	}

	NetworkTrackerReceiver& receiver = NetworkTrackerReceiver::Instance();
	NetTrackerSample s;
	NetTrackerFrameSample frame;
	if (!receiver.GetTracker(trackerIdx, s, &frame))
		return;
	const uint64_t sampleNow = NetworkTrackerReceiver::NowMs();
	const bool frameFresh = frame.everSeen && sampleNow >= frame.lastUpdateMs
	    && sampleNow - frame.lastUpdateMs <= 1500;
	// A framed sender commits tracker samples and policy together. If its frame
	// heartbeat stops, do not fall back to legacy gait heuristics for stale data.
	if (frame.everSeen && !frameFresh)
		return;
	if (frameFresh && !receiver.IsAlignmentReady(frame.sourceEpoch))
		return;
	if (sampleNow < s.lastUpdateMs || sampleNow - s.lastUpdateMs > 1500)
		return; // sender went quiet — report untracked rather than freeze

	if (frameFresh
	    && (!lastFrameEpochValid || lastFrameEpoch != frame.sourceEpoch)) {
		lastFrameEpoch = frame.sourceEpoch;
		lastFrameEpochValid = true;
		kickPassthroughUntilMs = 0;
		kickRecoveryDeadlineMs = 0;
	}
	const bool sourceUsesCameraFootArbitration = !frame.everSeen
	    || NetTrackerFrameUsesCameraFootArbitration(frame.flags);
	const bool cameraFoot = trackerIdx == 1 || trackerIdx == 2;
	if (sourceUsesCameraFootArbitration)
		CameraLegCalibration::Tick();
	uint64_t arbitrationNow = 0;
	NetCameraLegSample leg;
	bool semanticFresh = false;
	bool calibrationGrounded = false;

	// Consume camera semantics even before locomotion begins. If a confirmed
	// kick starts while standing and stick/WIP locomotion engages during its
	// recovery, that leg must not suddenly disappear from FBT. Timers are
	// per XrNetworkTracker instance, so the other foot can still yield to VRIK.
	if (sourceUsesCameraFootArbitration && cameraFoot) {
		arbitrationNow = NetworkTrackerReceiver::NowMs();
		semanticFresh = receiver.GetCameraLeg(trackerIdx - 1, leg, 500);
		semanticFresh = semanticFresh
		    && (!frame.everSeen
		        || (leg.sourceEpoch == frame.sourceEpoch
		            && leg.frameGeneration == frame.generation));
		if (semanticFresh && leg.state != NetCameraLegState::Invalid
		    && leg.confidence < 0.08f)
			leg.state = NetCameraLegState::Invalid;
		if (semanticFresh) {
			calibrationGrounded = leg.state == NetCameraLegState::Grounded;
			if (leg.state == NetCameraLegState::KickExtend) {
				kickPassthroughUntilMs = arbitrationNow + 450;
				kickRecoveryDeadlineMs = arbitrationNow + 1600;
			} else if (leg.state == NetCameraLegState::Recover
			    && arbitrationNow < kickRecoveryDeadlineMs) {
				kickPassthroughUntilMs = arbitrationNow + 180;
			} else if (leg.state == NetCameraLegState::Grounded
			    || leg.state == NetCameraLegState::KneeHold
			    || leg.state == NetCameraLegState::WalkStep) {
				kickPassthroughUntilMs = 0;
				kickRecoveryDeadlineMs = 0;
			}
		}
		if (!semanticFresh) {
			const float rawSpeed = std::sqrt(s.vel[0] * s.vel[0]
			    + s.vel[1] * s.vel[1] + s.vel[2] * s.vel[2]);
			calibrationGrounded = s.pos[1] < 0.12f && rawSpeed < 0.20f;
		}
	}

	// NET2/NET3 are the camera feet. While a real or synthesized locomotion
	// stick is active, ordinary planted/marching poses yield to VRIK's walk
	// cycle instead of pinning/gliding the feet over the room. Legacy 2D opts in
	// explicitly; Continuous3D is the direct camera path and participates too.
	// A confirmed kick/recovery keeps only that leg live for FBT/HIGGS. Physical
	// HTCX/Vive trackers already returned above and never enter this branch.
	if (allowLocomotionRelease && sourceUsesCameraFootArbitration && cameraFoot) {
		BaseInput* input = GetUnsafeBaseInput();
		if (input && input->ShouldReleaseNetworkFeetForLocomotion()) {
			if (!semanticFresh) {
				// Backward compatibility for generic OSC senders that do not provide
				// OCU's semantic gait packets. Continuous World3D uses this geometric
				// fallback until its matching leg-state stream is published.
				float speed = std::sqrt(s.vel[0] * s.vel[0] + s.vel[1] * s.vel[1]
				    + s.vel[2] * s.vel[2]);
				bool forwardStrike = s.pos[2] > 0.14f && s.vel[2] > 0.30f;
				bool fastHighStrike = s.pos[1] > 0.32f && speed > 1.0f;
				bool deliberateKick = s.pos[1] > 0.18f && speed > 0.75f
				    && (forwardStrike || fastHighStrike);
				if (deliberateKick)
					kickPassthroughUntilMs = arbitrationNow + 250;
			}
			if (arbitrationNow >= kickPassthroughUntilMs)
				return;
		}
	}

	// Sender convention is Unity: left-handed, y-up, +z forward, meters.
	// OpenXR is right-handed, y-up, -z forward: flip z on vectors.
	glm::vec3 pos(s.pos[0], s.pos[1], -s.pos[2]);
	glm::vec3 vel(s.vel[0], s.vel[1], -s.vel[2]);

	// Unity Quaternion.Euler(x,y,z) = Qy(y)*Qx(x)*Qz(z), degrees. Reproduce
	// that quaternion numerically, then reinterpret it in the z-flipped
	// right-handed frame, which negates the x and y components.
	float rx = glm::radians(s.eulerDeg[0]);
	float ry = glm::radians(s.eulerDeg[1]);
	float rz = glm::radians(s.eulerDeg[2]);
	glm::quat qU = glm::angleAxis(ry, glm::vec3(0, 1, 0))
	    * glm::angleAxis(rx, glm::vec3(1, 0, 0))
	    * glm::angleAxis(rz, glm::vec3(0, 0, 1));
	glm::quat q(qU.w, -qU.x, -qU.y, qU.z);

	// Map sender space into our playspace: auto height scale, camera/body-frame
	// -> HMD yaw alignment, then root offset. All remain
	// identity/zero for legacy OSC senders without the relevant head reference.
	float scale = NetworkTrackerReceiver::Instance().GetAlignmentScale();
	float alignYaw = NetworkTrackerReceiver::Instance().GetAlignmentYaw();
	float off[3];
	NetworkTrackerReceiver::Instance().GetAlignmentOffset(off);
	glm::quat qAlign = glm::angleAxis(alignYaw, glm::vec3(0, 1, 0));
	const glm::vec3 alignmentOffset(off[0], off[1], off[2]);
	pos = qAlign * (pos * scale) + alignmentOffset;
	vel = qAlign * (vel * scale);
	q = qAlign * q;
	glm::vec3 cameraWaist(0.0f);
	const bool cameraWaistValid = sourceUsesCameraFootArbitration
	    && GetAlignedCameraWaist(receiver, frame, scale, qAlign,
	        alignmentOffset, cameraWaist);
	// Editing is always headset-rooted. This common translation is applied to
	// the waist, feet, knees, elbows and chest, so the body cannot pop forward
	// or sideways while one foot is being trimmed. Raw locomotion reads bypass
	// it, and physical Vive/Tundra tracker poses returned at the top of this call.
	if (allowLocomotionRelease && sourceUsesCameraFootArbitration) {
		glm::vec3 rootCorrection;
		if (cameraWaistValid
		    && GetLiveCameraRootCorrection(cameraWaist, rootCorrection)) {
			pos += rootCorrection;
			cameraWaist += rootCorrection;
		}
	}
	// The in-VR calibration changes only the public camera foot targets. Raw
	// locomotion reads remain untouched so a visual foot trim cannot alter gait
	// thresholds or create synthetic walking. Physical trackers returned above.
	if (allowLocomotionRelease && sourceUsesCameraFootArbitration && cameraFoot)
		CameraLegCalibration::Apply(trackerIdx - 1, pos, vel, q, qAlign,
		    cameraWaist, cameraWaistValid, calibrationGrounded);

	glm::mat4 inFloor = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(q);

	// Network poses live in floor (standing) space; rebase for other origins
	glm::mat4 mat = inFloor;
	XrSpace base = xr_space_from_tracking_origin(origin);
	if (base != xr_gbl->floorSpace) {
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->floorSpace, base, xr_gbl->GetBestTime(), &loc))
		    && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			mat = X2G_om34_pose(loc.pose) * inFloor;
	}

	pose->mDeviceToAbsoluteTracking = G2S_m34(mat);
	pose->vVelocity.v[0] = vel.x; // world-space, needed for FBT kick physics
	pose->vVelocity.v[1] = vel.y;
	pose->vVelocity.v[2] = vel.z;
	pose->bPoseIsValid = true;
	pose->eTrackingResult = vr::TrackingResult_Running_OK;
}

uint32_t XrNetworkTracker::GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop,
    char* value, uint32_t bufferSize, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

#define PROP(in, out)                                                \
	if (prop == in) {                                                \
		if (value != NULL && bufferSize > 0) {                       \
			strcpy_s(value, bufferSize, out);                        \
		}                                                            \
		return (uint32_t)strlen(out) + 1;                            \
	}

	PROP(vr::Prop_SerialNumber_String, serial);

	// Present the same identity surface as a Vive tracker. Falling through to
	// XrTrackedDevice here would report "oculus", creating contradictory
	// properties even though the device class/controller type are tracker.
	PROP(vr::Prop_TrackingSystemName_String, "lighthouse");
	PROP(vr::Prop_ManufacturerName_String, "HTC");
	PROP(vr::Prop_ControllerType_String, "vive_tracker");
	PROP(vr::Prop_ModelNumber_String, "Vive Tracker Pro MV");
	PROP(vr::Prop_RenderModelName_String, "{htc}vr_tracker_vive_1_0");
	PROP(vr::Prop_RegisteredDeviceType_String, "htc/vive_tracker");
	PROP(vr::Prop_InputProfilePath_String, "{htc}/input/vive_tracker_profile.json");

#undef PROP

	return XrTrackedDevice::GetStringTrackedDeviceProperty(prop, value, bufferSize, pErrorL);
}

vr::ETrackedDeviceClass XrNetworkTracker::GetTrackedDeviceClass()
{
	return vr::TrackedDeviceClass_GenericTracker;
}
