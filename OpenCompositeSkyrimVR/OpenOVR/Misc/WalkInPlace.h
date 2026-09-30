#pragma once

// Walk-in-place locomotion: stepping in place (read from ANY body-tracker
// source — camera, SlimeVR, pucks) synthesizes forward stick input; cadence
// controls speed. Steps are detected from the LEFT-RIGHT ALTERNATION of the
// feet (anti-phase, double amplitude, common-mode camera noise cancels — a
// casual walk barely shows per-foot lift by the time it reaches us) with the
// per-foot lift detector kept for single-foot sources. Camera feet require
// opposed camera-skeleton arm corroboration; trusted HTCX/Vive feet can confirm
// repeated gait alone. Direction: both hands held at ~chin height = backwards,
// using skeleton hands first and controller height in tracker-only mode.
// Movement is injected into the left stick and palm steering into the right
// stick by BaseInput; real stick input overrides.
//
// Ini: [input] walkInPlaceEnabled=false / walkInPlaceSpeed=1.0 /
// walkInPlaceActivation=none

#include <atomic>
#include <cstdint>
#include <vector>

#include "NetworkTrackers.h"

class WalkInPlace {
public:
	static WalkInPlace& Instance();

	// Called once per frame pump (world/standing space, meters).
	// cameraSemanticGait marks the Continuous3D camera contract: metric geometry
	// owns positive gait/cadence evidence while semantic action states provide a
	// kick/knee-hold veto. A true HTCX/Vive foot pair bypasses that camera policy.
	void Update(bool lValid, float lFootY, bool rValid, float rFootY,
	    bool lkValid, float lKneeY, bool rkValid, float rKneeY,
	    bool lHardwareFoot, bool rHardwareFoot,
	    bool cameraSemanticGait,
	    bool lCameraLegValid, NetCameraLegState lCameraLegState,
	    bool rCameraLegValid, NetCameraLegState rCameraLegState,
	    bool hmdValid, float hmdY,
	    float hmdForwardX, float hmdForwardZ,
	    bool skeletonArmsValid, float skeletonArmPhase, float lSkeletonHandY, float rSkeletonHandY,
	    bool lTurnCtrlValid, float lCtrlHeadX, float lCtrlHeadZ, float lPalmFrontX, float lPalmFrontZ, float lCtrlY,
	    bool rTurnCtrlValid, float rCtrlHeadX, float rCtrlHeadZ, float rPalmFrontX, float rPalmFrontZ, float rCtrlY,
	    bool explicitActivationHeld);

	// -1..1 forward axis (negative = backwards). Thread-safe; 0 when idle.
	float AxisY() const { return axisOut.load(); }

	// -1..1 turn-stick axis (negative = left, positive = right).
	float TurnX() const { return turnOutAtomic.load(); }
	bool IsRunning() const { return runningOut.load(); }

#ifdef OCU_RUNTIME_SELF_TEST
	// Test-only construction and clock control keep semantic transition tests
	// deterministic. They are absent from the production class definition.
	static WalkInPlace* TestCreate();
	static void TestDestroy(WalkInPlace* instance);
	static void TestSetNowMs(uint64_t nowMs);
	bool TestCameraKickLatched(int side) const;
	bool TestQuickStartArmed() const { return quickStartUntilMs != 0; }
#endif

private:
	WalkInPlace() = default;

	static uint64_t NowMs();

	// Per-foot step detection state
	float footFilt[2] = {}; // spike-blunted foot height (rate-limited input)
	float baseline[2] = {};
	bool baselineInit[2] = {};
	bool footUp[2] = {};
	uint64_t footUpAt[2] = {}; // stuck-guard: no real step lasts >1.5s
	float minSinceUp[2] = {}; // lowest y while "up" = best floor estimate
	uint64_t lastUpdateMs = 0; // for framerate-independent filters
	uint64_t lastFeetSourceMs = 0;
	uint64_t lastSkeletonSourceMs = 0;
	uint64_t feetRecoveryQuarantineUntilMs = 0; // no gait while a camera pose relocks
	float lastRawFootY[2] = {};
	bool rawFeetInit = false;

	// Feet/knee-alternation step counter (primary when both legs track)
	float altDiffSlow = 0.0f; // slow center of (L-R) ankles, absorbs offset
	bool altDiffInit = false;
	int altState = 0; // -1 / 0 / +1 hysteresis state of the centered diff
	float kneeFilt[2] = {}; // spike-blunted knee heights
	bool kneeInit[2] = {};
	float kneeDiffSlow = 0.0f; // slow center of (L-R) knees
	bool kneeDiffInit = false;

	// Camera-skeleton arm corroborator. The Configurator fuses shoulder, elbow,
	// and wrist motion into a signed left-vs-right phase in meters.
	float armDiffSlow = 0.0f;
	bool armDiffInit = false;
	int armState = 0;
	std::vector<uint64_t> armTimes; // recent swing-crossing timestamps
	uint64_t lastSwingMs = 0;
	uint64_t lastArmMotionMs = 0; // slow gait may have motion without full phase crossings
	float previousArmPhase = 0.0f;
	bool armPhaseInit = false;
	uint64_t lastArmPhaseSampleMs = 0; // camera phase is held between its much slower inference frames
	float armSwingSpeed = 0.0f; // EMA of skeleton phase speed (m/s)

	std::vector<uint64_t> stepTimes; // recent step timestamps
	uint64_t lastStepMs = 0;
	uint64_t lastFootLiftMs[2] = {}; // proof that the side reported by fused phase actually lifted
	int lastRecordedStepSide = -1; // prevents one leg plus baseline rebound from impersonating alternation
	uint64_t lastClearFootLiftMs = 0;
	uint64_t lastHighFootLiftMs = 0; // deliberate ~14cm+ lift, used for camera run intent
	uint64_t lastStrongArmMs = 0; // preserves a strong pump across one missed camera frame
	bool gaitConfirmed = false; // true only after a plausible alternating rhythm
	uint64_t gaitReleaseSinceMs = 0; // short debounce after both leg/arm evidence actually expires
	uint64_t quickStartUntilMs = 0; // selected hold button + one clear foot lift
	uint64_t runCandidateSinceMs = 0; // sustained fast cadence; rejects one camera/arm spike
	uint64_t runModeUntilMs = 0; // short hysteresis across a missed run crossing
	NetCameraLegState lastCameraLegState[2] = {
		NetCameraLegState::Invalid, NetCameraLegState::Invalid
	};
	bool cameraLegStateInit[2] = {};
	bool cameraKickLatched[2] = {}; // confirmed kick remains isolated through recovery
	uint64_t cameraKickLastSeenMs[2] = {};
	uint64_t cameraGroundedSinceMs[2] = {};
	uint64_t lastCameraWalkSemanticMs = 0; // World3D WalkStep corroboration

	bool goingBackward = false; // direction, latched while marching
	uint64_t handsHighSince = 0; // sustained-raise timer for engaging the latch
	uint64_t handsBelowSince = 0; // sustained-drop timer for releasing the latch
	float outSpeed = 0.0f; // smoothed 0..1
	float recentPeakLift = 0.0f; // decayed max foot lift; amplitude intent for speed
	bool turnGesture[2] = {}; // left/right palm held near 90 degrees from neutral
	uint64_t turnGestureSince[2] = {};
	float turnOut = 0.0f;

	std::atomic<float> axisOut{ 0.0f };
	std::atomic<float> turnOutAtomic{ 0.0f };
	std::atomic<bool> runningOut{ false };
};
