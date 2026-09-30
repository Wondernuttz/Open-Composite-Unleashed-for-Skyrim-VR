#include "stdafx.h"

#include "CameraLegCalibration.h"

#include "Config.h"
#include "xrmoreutils.h"
#include "../logging.h"
#include "../Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>

namespace CameraLegCalibration {
namespace {

struct LegTrim {
	glm::vec3 position{ 0.0f };
	float liftGain = 1.0f;
	float sideGain = 1.0f;
	float forwardGain = 1.0f;
	glm::vec3 rotationDeg{ 0.0f }; // pitch, yaw, roll in camera/body axes
};

std::once_flag g_loadOnce;
std::mutex g_trimMutex;
std::mutex g_tickMutex;
LegTrim g_trim[2];
glm::vec3 g_neutralLocal[2]{ glm::vec3(0.0f), glm::vec3(0.0f) };
bool g_neutralLocalValid[2]{ false, false };
std::atomic<bool> g_active{ false };
bool g_lastRightA = false;
int g_rightATapCount = 0;
uint64_t g_lastRightATapMs = 0;
bool g_dirty = false;
uint64_t g_lastTickMs = 0;
uint64_t g_lastSaveMs = 0;

uint64_t NowMs()
{
	return GetTickCount64();
}

std::string CalibrationPath()
{
	char modulePath[MAX_PATH]{};
	GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
	std::string path(modulePath);
	size_t slash = path.find_last_of("\\/");
	if (slash != std::string::npos)
		path.resize(slash + 1);
	else
		path.clear();
	return path + "camera_leg_calibration.ini";
}

float ReadFloat(const char* section, const char* key, float fallback)
{
	char fallbackText[32]{};
	char value[64]{};
	snprintf(fallbackText, sizeof(fallbackText), "%.6f", fallback);
	GetPrivateProfileStringA(section, key, fallbackText, value, sizeof(value),
	    CalibrationPath().c_str());
	char* end = nullptr;
	float parsed = strtof(value, &end);
	return end != value && std::isfinite(parsed) ? parsed : fallback;
}

void Load()
{
	std::lock_guard<std::mutex> lock(g_trimMutex);
	for (int i = 0; i < 2; ++i) {
		const char* section = i == 0 ? "left_foot" : "right_foot";
		g_trim[i].position.x = ReadFloat(section, "side", 0.0f);
		// A constant vertical offset makes one posed knee look right but leaves
		// that foot floating when the user stands again. Vertical calibration is
		// now a motion gain around the camera's floor-relative zero instead.
		g_trim[i].position.y = 0.0f;
		g_trim[i].position.z = ReadFloat(section, "forward", 0.0f);
		g_trim[i].liftGain = std::clamp(
		    ReadFloat(section, "lift_gain", 1.0f), 0.75f, 3.0f);
		g_trim[i].sideGain = std::clamp(
		    ReadFloat(section, "side_gain", 1.0f), 0.60f, 3.0f);
		g_trim[i].forwardGain = std::clamp(
		    ReadFloat(section, "forward_gain", 1.0f), 0.50f, 3.0f);
		// Rotation is an explicit part of live foot calibration. It changes only
		// the tracker orientation; position stays on the raw camera target.
		g_trim[i].rotationDeg.x = std::clamp(
		    ReadFloat(section, "pitch", 0.0f), -75.0f, 75.0f);
		g_trim[i].rotationDeg.y = std::clamp(
		    ReadFloat(section, "yaw", 0.0f), -120.0f, 120.0f);
		g_trim[i].rotationDeg.z = std::clamp(
		    ReadFloat(section, "roll", 0.0f), -75.0f, 75.0f);
	}
	OOVR_LOGF("Camera leg calibration loaded: L pos=(%+.3f,%+.3f,%+.3f) lift=%.2fx side=%.2fx forward=%.2fx rot=(%+.1f,%+.1f,%+.1f), R pos=(%+.3f,%+.3f,%+.3f) lift=%.2fx side=%.2fx forward=%.2fx rot=(%+.1f,%+.1f,%+.1f)",
	    g_trim[0].position.x, g_trim[0].position.y, g_trim[0].position.z,
	    g_trim[0].liftGain, g_trim[0].sideGain, g_trim[0].forwardGain,
	    g_trim[0].rotationDeg.x, g_trim[0].rotationDeg.y, g_trim[0].rotationDeg.z,
	    g_trim[1].position.x, g_trim[1].position.y, g_trim[1].position.z,
	    g_trim[1].liftGain, g_trim[1].sideGain, g_trim[1].forwardGain,
	    g_trim[1].rotationDeg.x, g_trim[1].rotationDeg.y, g_trim[1].rotationDeg.z);
}

void EnsureLoaded()
{
	std::call_once(g_loadOnce, Load);
}

void Save()
{
	LegTrim snapshot[2];
	{
		std::lock_guard<std::mutex> lock(g_trimMutex);
		snapshot[0] = g_trim[0];
		snapshot[1] = g_trim[1];
	}

	const std::string path = CalibrationPath();
	FILE* file = fopen(path.c_str(), "w");
	if (!file) {
		OOVR_LOGF("Camera leg calibration: could not save %s", path.c_str());
		return;
	}
	for (int i = 0; i < 2; ++i) {
		fprintf(file,
		    "[%s]\nside=%.6f\nheight=0.000000\nforward=%.6f\nlift_gain=%.4f\nside_gain=%.4f\nforward_gain=%.4f\npitch=%.4f\nyaw=%.4f\nroll=%.4f\n\n",
		    i == 0 ? "left_foot" : "right_foot",
		    snapshot[i].position.x, snapshot[i].position.z, snapshot[i].liftGain,
		    snapshot[i].sideGain, snapshot[i].forwardGain,
		    snapshot[i].rotationDeg.x, snapshot[i].rotationDeg.y,
		    snapshot[i].rotationDeg.z);
	}
	fclose(file);
	g_dirty = false;
	g_lastSaveMs = NowMs();
	OOVR_LOGF("Camera leg calibration saved: %s", path.c_str());
}

bool ButtonDown(const vr::VRControllerState_t& state, vr::EVRButtonId button)
{
	return (state.ulButtonPressed & vr::ButtonMaskFromId(button)) != 0;
}

float Deadzone(float value)
{
	constexpr float threshold = 0.16f;
	if (std::abs(value) <= threshold)
		return 0.0f;
	// Remove the discontinuity at the edge while retaining full-scale travel.
	return std::copysign((std::abs(value) - threshold) / (1.0f - threshold), value);
}

void Reset()
{
	{
		std::lock_guard<std::mutex> lock(g_trimMutex);
		g_trim[0] = {};
		g_trim[1] = {};
		g_neutralLocalValid[0] = false;
		g_neutralLocalValid[1] = false;
		g_dirty = true;
	}
	Save();
	OOVR_LOG("Camera leg calibration RESET to identity");
}

void AdjustLeg(int index, float stickX, float stickY, bool heightModifier,
    bool gripModifier, float dt)
{
	stickX = Deadzone(stickX);
	stickY = Deadzone(stickY);
	if (stickX == 0.0f && stickY == 0.0f)
		return;

	constexpr float moveSpeed = 0.12f;
	constexpr float liftGainSpeed = 0.90f; // multiplier change per second
	constexpr float rotateSpeed = 70.0f; // degrees per second
	LegTrim& trim = g_trim[index];
	if (gripModifier) {
		// Grip turns the same-side virtual tracker instead of moving it. X is
		// foot yaw/turn and Y is pitch/pivot. This is orientation-only: it must
		// never rotate the tracked ankle position around the pelvis.
		trim.rotationDeg.y = std::clamp(
		    trim.rotationDeg.y + stickX * rotateSpeed * dt, -120.0f, 120.0f);
		trim.rotationDeg.x = std::clamp(
		    trim.rotationDeg.x + stickY * rotateSpeed * dt, -75.0f, 75.0f);
	} else if (heightModifier) {
		// X/trigger alone owns lift range. Grip intentionally takes precedence
		// when both are held because grip is the explicit rotation modifier.
		const float speed = liftGainSpeed;
		trim.liftGain = std::clamp(trim.liftGain + stickY * speed * dt,
		    0.75f, 3.0f);
	} else {
		// Unmodified sticks are the live placement tool: each stick moves the
		// same-side foot immediately in the horizontal plane. Do not hide this
		// behind motion gains; the player must be able to line the avatar up by
		// sight while standing in VR.
		trim.position.x = std::clamp(trim.position.x + stickX * moveSpeed * dt,
		    -0.45f, 0.45f);
		trim.position.z = std::clamp(trim.position.z - stickY * moveSpeed * dt,
		    -0.45f, 0.45f);
	}
	g_dirty = true;
}

} // namespace

bool CapturesInput()
{
	return g_active.load(std::memory_order_acquire);
}

void Tick()
{
	std::lock_guard<std::mutex> tickLock(g_tickMutex);
	EnsureLoaded();
	if (!oovr_global_configuration.CameraLegCalibrationEnabled()) {
		if (g_active.exchange(false)) {
			if (g_dirty) Save();
			OOVR_LOG("Camera leg calibration controls disabled by opencomposite.ini");
		}
		return;
	}

	const uint64_t now = NowMs();
	if (now == g_lastTickMs)
		return;
	float dt = g_lastTickMs == 0 ? 0.0f : (now - g_lastTickMs) / 1000.0f;
	g_lastTickMs = now;
	dt = std::clamp(dt, 0.0f, 0.05f);

	BaseInput* input = GetUnsafeBaseInput();
	if (!input)
		return;

	vr::VRControllerState_t left{}, right{};
	if (!input->GetLegacyControllerState(1, &left)
	    || !input->GetLegacyControllerState(2, &right))
		return;

	const bool xHeld = ButtonDown(left, vr::k_EButton_A);
	const bool leftTrigger = left.rAxis[1].x > 0.55f
	    || ButtonDown(left, vr::k_EButton_SteamVR_Trigger);
	const bool rightTrigger = right.rAxis[1].x > 0.55f
	    || ButtonDown(right, vr::k_EButton_SteamVR_Trigger);
	// Some Meta/OpenXR binding sets do not surface X through the legacy A bit.
	// Either trigger is an explicit, unambiguous fallback while calibration
	// owns and masks game input.
	const bool liftModifier = xHeld || leftTrigger || rightTrigger;
	const bool rightA = ButtonDown(right, vr::k_EButton_A);
	constexpr uint64_t tapWindowMs = 900;
	if (g_rightATapCount > 0 && now - g_lastRightATapMs > tapWindowMs)
		g_rightATapCount = 0;

	// Triple-tap RIGHT A to toggle. Stick clicks are deliberately never used:
	// Skyrim and several mods bind menus/actions to them. A held button counts
	// once because only the rising edge enters this block.
	if (rightA && !g_lastRightA) {
		if (g_rightATapCount == 0 || now - g_lastRightATapMs <= tapWindowMs)
			++g_rightATapCount;
		else
			g_rightATapCount = 1;
		g_lastRightATapMs = now;

		if (g_rightATapCount >= 3) {
			g_rightATapCount = 0;
			if (liftModifier) {
				Reset();
			} else {
				const bool active = !g_active.load(std::memory_order_relaxed);
				g_active.store(active, std::memory_order_release);
				if (!active && g_dirty)
					Save();
				OOVR_LOGF("Camera leg calibration edit mode: %s", active ? "ON" : "OFF");
			}
		}
	}
	g_lastRightA = rightA;

	if (!g_active.load(std::memory_order_acquire) || dt <= 0.0f)
		return;

	const bool leftGrip = left.rAxis[2].x > 0.50f
	    || ButtonDown(left, vr::k_EButton_Grip);
	const bool rightGrip = right.rAxis[2].x > 0.50f
	    || ButtonDown(right, vr::k_EButton_Grip);
	{
		std::lock_guard<std::mutex> lock(g_trimMutex);
		// Each controller edits the foot on that same side. Only the lateral
		// stick axis is mirrored by the controller/avatar coordinate boundary,
		// so invert X here; forward/back (Y) is already correct.
		AdjustLeg(0, -left.rAxis[0].x, left.rAxis[0].y, liftModifier, leftGrip, dt);
		AdjustLeg(1, -right.rAxis[0].x, right.rAxis[0].y, liftModifier, rightGrip, dt);
	}

	if (g_dirty && now - g_lastSaveMs >= 1500)
		Save();
}

void Apply(int footIndex, glm::vec3& position, glm::vec3& velocity,
    glm::quat& orientation, const glm::quat& alignment,
    const glm::vec3& bodyRoot, bool bodyRootValid, bool grounded)
{
	EnsureLoaded();
	if (footIndex < 0 || footIndex > 1)
		return;

	LegTrim trim;
	{
		std::lock_guard<std::mutex> lock(g_trimMutex);
		trim = g_trim[footIndex];
	}

	// Tracker geometry is already in its final playspace direction. Calibration
	// may change vertical reach and a fixed visual trim, but it must never mirror,
	// rotate or rebase live X/Z motion after the native skeleton conversion.
	if (position.y > 0.0f)
		position.y *= trim.liftGain;
	velocity.y *= trim.liftGain;
	position += alignment * glm::vec3(trim.position.x, 0.0f, trim.position.z);

	// Apply the requested foot orientation about the tracker itself. Conjugating
	// by the fixed camera alignment expresses pitch/yaw/roll in body axes without
	// using HMD look direction and without moving the ankle target.
	const glm::quat localRotation =
	    glm::angleAxis(glm::radians(trim.rotationDeg.y), glm::vec3(0, 1, 0))
	    * glm::angleAxis(glm::radians(trim.rotationDeg.x), glm::vec3(1, 0, 0))
	    * glm::angleAxis(glm::radians(trim.rotationDeg.z), glm::vec3(0, 0, 1));
	const glm::quat worldRotation = alignment * localRotation * glm::inverse(alignment);
	orientation = glm::normalize(worldRotation * orientation);
	(void)bodyRoot;
	(void)bodyRootValid;
	(void)grounded;
}

} // namespace CameraLegCalibration
