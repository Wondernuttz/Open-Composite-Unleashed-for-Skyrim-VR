#include "XrController.h"

// HACK: grab the pose from BaseInput
#include "../OpenOVR/Misc/xrmoreutils.h"
#include "../OpenOVR/Misc/Config.h"
#include "../OpenOVR/Misc/CameraLegCalibration.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"

#include <algorithm>
#include <glm/gtx/transform.hpp>

// ── Live render-model trim adjustment (2026-07-26, renderModelAdjust=true) ──
// Stick-driven, fully native, per-frame in the pose path (no Papyrus VM, no
// scripting): LEFT stick up/down = raise/lower, left/right = forward/back,
// either grip held switches left/right to sideways strafe. RIGHT stick
// left/right = roll, up/down = nose pitch, grip held + left/right = yaw.
// Movements are WORLD-intuitive (computed against the live hold orientation),
// accumulated as a device-space trim so it stays glued to the controller.
// The equivalent opencomposite.ini values print to the log every 2s as
// "RM-ADJUST ..." — dial until perfect, quit, paste the last line.
// While enabled, the trim applies to the CONTROLLER POSE (game caches the
// mesh at load, so live feedback must ride the pose); the identical values
// baked into renderModel* keys apply to the mesh on the next launch.
static glm::mat4 s_rmAdjust(1.0f); // device-space trim, right-hand convention

static void RmAdjustTick(const glm::mat3& rNow)
{
	if (CameraLegCalibration::CapturesInput())
		return;
	BaseInput* input = GetUnsafeBaseInput();
	if (!input)
		return;

	static ULONGLONG s_lastTick = 0;
	ULONGLONG now = GetTickCount64();
	float dt = (s_lastTick == 0) ? 0.0f : (now - s_lastTick) / 1000.0f;
	s_lastTick = now;
	if (dt <= 0.0f || dt > 0.25f)
		return;

	// Device indices: 1 = left controller, 2 = right controller
	vr::VRControllerState_t stL = {}, stR = {};
	input->GetLegacyControllerState(1, &stL);
	input->GetLegacyControllerState(2, &stR);

	auto dz = [](float v) { return fabsf(v) < 0.15f ? 0.0f : v; };
	float lx = dz(stL.rAxis[0].x), ly = dz(stL.rAxis[0].y);
	float rx = dz(stR.rAxis[0].x), ry = dz(stR.rAxis[0].y);
	bool grip = stL.rAxis[2].x > 0.5f || stR.rAxis[2].x > 0.5f;
	if (lx == 0.0f && ly == 0.0f && rx == 0.0f && ry == 0.0f)
		return;

	constexpr float kMoveSpeed = 0.05f; // m/s at full deflection
	constexpr float kRotSpeed = 40.0f * 3.14159265f / 180.0f; // rad/s

	// World-intuitive axes from the live hold orientation
	glm::vec3 up(0, 1, 0);
	glm::vec3 fwd = rNow * glm::vec3(0, 0, -1);
	fwd.y = 0.0f;
	if (glm::length(fwd) > 1e-4f)
		fwd = glm::normalize(fwd);
	glm::vec3 right = rNow * glm::vec3(1, 0, 0);
	right.y = 0.0f;
	if (glm::length(right) > 1e-4f)
		right = glm::normalize(right);

	// Translation increment in world space
	glm::vec3 dWorld = up * (ly * kMoveSpeed * dt);
	if (grip)
		dWorld += right * (lx * kMoveSpeed * dt); // strafe
	else
		dWorld += fwd * (-lx * kMoveSpeed * dt); // stick-left = forward

	// Rotation increment in world space
	glm::mat3 dRotW(1.0f);
	if (rx != 0.0f) {
		glm::vec3 axis = grip ? up : glm::vec3(rNow * glm::vec3(0, 0, -1)); // yaw with grip, else roll
		dRotW = glm::mat3(glm::rotate(rx * kRotSpeed * dt, axis)) * dRotW;
	}
	if (ry != 0.0f) {
		glm::vec3 lat = rNow * glm::vec3(1, 0, 0); // nose pitch about the lateral axis
		dRotW = glm::mat3(glm::rotate(ry * kRotSpeed * dt, lat)) * dRotW;
	}

	// Conjugate the world-space delta into device space and accumulate
	glm::mat3 rT = glm::transpose(rNow);
	glm::mat3 dRotDev = rT * dRotW * rNow;
	glm::vec3 dPosDev = rT * dWorld;
	glm::mat4 delta(1.0f);
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			delta[c][r] = dRotDev[c][r];
	delta[3] = glm::vec4(dPosDev, 1.0f);
	s_rmAdjust = delta * s_rmAdjust;

	// Print the equivalent ini values every 2s (translate*RotY*RotX*RotZ order)
	static ULONGLONG s_lastLog = 0;
	if (now - s_lastLog > 2000) {
		s_lastLog = now;
		// Row-major elements m[r][c] = glm [c][r]
		float m12 = s_rmAdjust[2][1]; // row1,col2
		float sx = -m12;
		sx = std::clamp(sx, -1.0f, 1.0f);
		float thX = asinf(sx);
		float thY = atan2f(s_rmAdjust[2][0], s_rmAdjust[2][2]); // m02, m22
		float thZ = atan2f(s_rmAdjust[0][1], s_rmAdjust[1][1]); // m10, m11
		constexpr float r2d = 180.0f / 3.14159265f;
		OOVR_LOGF("RM-ADJUST renderModelRotX=%.1f renderModelRotY=%.1f renderModelRotZ=%.1f renderModelOffX=%.4f renderModelOffY=%.4f renderModelOffZ=%.4f",
		    thX * r2d, thY * r2d, thZ * r2d,
		    s_rmAdjust[3][0], s_rmAdjust[3][1], s_rmAdjust[3][2]);
	}
}

XrController::XrController(XrController::XrControllerType type, const InteractionProfile& profile)
    : type(type), profile(profile)
{
	InitialiseDevice(GetHand() + 1);
}

#define TRY_PROFILE_PROP(type)                                                \
	do {                                                                      \
		std::optional<type> ret = profile.GetProperty<type>(prop, GetHand()); \
		if (ret.has_value())                                                  \
			return *ret;                                                      \
	} while (0)

// properties
bool XrController::GetBoolTrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(bool);

	switch (prop) {
	case vr::Prop_DeviceProvidesBatteryStatus_Bool:
		return true;
	default:
		return XrTrackedDevice::GetBoolTrackedDeviceProperty(prop, pErrorL);
	}
}

int32_t XrController::GetInt32TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(int32_t);

	// Continue to pretend to be a CV1
	// Don't apply the inputs to the tracking object
	if (type == XCT_LEFT || type == XCT_RIGHT) {
		switch (prop) {
		case vr::Prop_Axis0Type_Int32:
			// TODO find out which of these SteamVR returns and do likewise
			// return vr::k_eControllerAxis_TrackPad;
			return vr::k_eControllerAxis_Joystick;

		case vr::Prop_Axis1Type_Int32:
		case vr::Prop_Axis2Type_Int32:
			return vr::k_eControllerAxis_Trigger;

		case vr::Prop_Axis3Type_Int32:
		case vr::Prop_Axis4Type_Int32:
			return vr::k_eControllerAxis_None;

		default:
			break;
		}
	}

	return XrTrackedDevice::GetInt32TrackedDeviceProperty(prop, pErrorL);
}

uint64_t XrController::GetUint64TrackedDeviceProperty(vr::ETrackedDeviceProperty prop, vr::ETrackedPropertyError* pErrorL)
{
	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	TRY_PROFILE_PROP(uint64_t);

	// This is for the old input system, which we don't initially need
	if (prop == vr::Prop_SupportedButtons_Uint64) {
		// Just assume we're an Oculus Touch-style controller and enable all the buttons.
		uint64_t supported = 0;
		supported |= vr::ButtonMaskFromId(vr::k_EButton_System);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_Grip);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_Axis2);
		if (profile.GetPath() == "/interaction_profiles/valve/frame_controller_valve")
			supported |= vr::ButtonMaskFromId(vr::k_EButton_Axis3);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Left);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Up);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Down);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Right);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_A);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad);
		supported |= vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger);
		return supported;
	}

	return XrTrackedDevice::GetUint64TrackedDeviceProperty(prop, pErrorL);
}

uint32_t XrController::GetStringTrackedDeviceProperty(vr::ETrackedDeviceProperty prop,
    char* value, uint32_t bufferSize, vr::ETrackedPropertyError* pErrorL)
{

	if (pErrorL)
		*pErrorL = vr::TrackedProp_Success;

	// Legacy meshes have their own stable, handed model names. Do this before
	// profile lookup: several profiles advertise a shared controller model name
	// with no left/right suffix, which cannot select an embedded hand mesh.
	std::optional<std::string> ret;
	if (prop == vr::Prop_RenderModelName_String && oovr_global_configuration.UseLegacyGreyHands()
	    && (type == XCT_LEFT || type == XCT_RIGHT)) {
		ret = type == XCT_LEFT ? "renderLeftHand" : "renderRightHand";
	} else {
		ret = profile.GetProperty<std::string>(prop, GetHand());
	}
	if (ret.has_value()) {
		const auto required = static_cast<uint32_t>(ret->size() + 1);
		if (value && bufferSize >= required) {
			memcpy(value, ret->c_str(), required);
		} else if (pErrorL) {
			*pErrorL = vr::TrackedProp_BufferTooSmall;
		}
		return required;
	}

#define PROP(in, out)                                                                                  \
	if (prop == in) {                                                                                  \
		if (value != NULL && bufferSize > 0) {                                                         \
			strcpy_s(value, bufferSize, out); /* FFS msvc - strncpy IS the secure version of strcpy */ \
		}                                                                                              \
		return (uint32_t)strlen(out) + 1;                                                              \
	}

	// Resonite determines controller type by using render model name, if it can't recognize - it will load generic controller
	switch (type) {
	case XCT_LEFT: {
		std::optional<const char*> leftHandPath = GetInteractionProfile()->GetLeftHandRenderModelName();
		if (leftHandPath.has_value()) {
			PROP(vr::Prop_RenderModelName_String, leftHandPath.value());
		} else {
			PROP(vr::Prop_RenderModelName_String, "renderLeftHand");
		}
		PROP(vr::Prop_RegisteredDeviceType_String, "oculus/F00BAAF00F_Controller_Left");
		break;
	}
	case XCT_RIGHT: {
		std::optional<const char*> rightHandPath = GetInteractionProfile()->GetRightHandRenderModelName();
		if (rightHandPath.has_value()) {
			PROP(vr::Prop_RenderModelName_String, rightHandPath.value());
		} else {
			PROP(vr::Prop_RenderModelName_String, "renderRightHand");
		}
		PROP(vr::Prop_RegisteredDeviceType_String, "oculus/F00BAAF00F_Controller_Right");
		break;
	}
	case XCT_TRACKED_OBJECT:
		PROP(vr::Prop_RenderModelName_String, "renderObject0");
		break;
	default:
		OOVR_ABORTF("Invalid controller type %d", type);
	}

	return XrTrackedDevice::GetStringTrackedDeviceProperty(prop, value, bufferSize, pErrorL);
}

ITrackedDevice::HandType XrController::GetHand()
{
	switch (type) {
	case XCT_LEFT:
		return HAND_LEFT;
	case XCT_RIGHT:
		return HAND_RIGHT;
	default:
		return HAND_NONE;
	}
}

void XrController::GetPose(vr::ETrackingUniverseOrigin origin, vr::TrackedDevicePose_t* pose, ETrackingStateType trackingState)
{
	// Default to an invalid pose
	ZeroMemory(pose, sizeof(*pose));
	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = false;
	pose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;

	BaseInput* input = GetUnsafeBaseInput();
	if (input == nullptr)
		return;

	// TODO do something with TrackingState
	XrSpace space = XR_NULL_HANDLE;

	// Specifically use grip pose, since that's what InteractionProfile::GetGripToSteamVRTransform uses
	GetBaseInput()->GetHandSpace(DeviceIndex(), space, false);

	if (!space) {
		// Lie and say the pose is valid if the actions haven't even been loaded yet.
		// This is a workaround for games like DCS, which appear to require valid poses before
		// it will even attempt to request the controller state (and thus create the actions).
		if (!input->AreActionsLoaded()) {
			pose->bPoseIsValid = true;
			pose->eTrackingResult = vr::TrackingResult_Running_OK;
		}
		return;
	}

	HandType hand = GetHand();

	// Find the hand transform matrix, and include that
	glm::mat4 transform = profile.GetGripToSteamVRTransform(hand);

	xr_utils::PoseFromSpace(pose, space, origin, transform, hand);

	// Live stick-driven trim (calibration mode, renderModelAdjust=true)
	if (pose->bPoseIsValid && oovr_global_configuration.RenderModelAdjust()) {
		auto& m = pose->mDeviceToAbsoluteTracking.m;
		glm::mat4 W(1.0f);
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				W[c][r] = m[r][c];

		if (hand != HAND_LEFT) {
			// Right hand drives the accumulator (once per frame)
			RmAdjustTick(glm::mat3(W));
		}

		glm::mat4 T = s_rmAdjust;
		if (hand == HAND_LEFT) {
			// Mirror across the X=0 plane: S*T*S (flips x-offset, yaw, roll)
			glm::mat4 S(1.0f);
			S[0][0] = -1.0f;
			T = S * T * S;
		}
		W = W * T;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 4; c++)
				m[r][c] = W[c][r];
	}
}

vr::ETrackedDeviceClass XrController::GetTrackedDeviceClass()
{
	return vr::TrackedDeviceClass_Controller;
}

const InteractionProfile* XrController::GetInteractionProfile()
{
	return &profile;
}
