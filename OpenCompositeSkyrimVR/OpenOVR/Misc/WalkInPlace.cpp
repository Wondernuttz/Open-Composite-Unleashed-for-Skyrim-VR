#ifdef OCU_RUNTIME_SELF_TEST
#include "WalkInPlace.h"

#define OOVR_LOGF(...) ((void)0)
static bool oovr_debug_logging_enabled() { return false; }
#else
#include "stdafx.h"

#include "WalkInPlace.h"

#include "Config.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

#ifdef OCU_RUNTIME_SELF_TEST
uint64_t testNowMs = 1;
#endif

constexpr bool IsCameraWalkState(NetCameraLegState state)
{
	return state == NetCameraLegState::WalkStep;
}

constexpr bool IsCameraActionPrelude(NetCameraLegState state)
{
	return state == NetCameraLegState::Chamber
	    || state == NetCameraLegState::KneeHold;
}

constexpr bool IsCameraConfirmedKick(NetCameraLegState state)
{
	return state == NetCameraLegState::KickExtend;
}

constexpr bool IsCameraRecoveryState(NetCameraLegState state)
{
	return state == NetCameraLegState::Recover;
}

constexpr bool IsCameraGroundState(NetCameraLegState state)
{
	return state == NetCameraLegState::Grounded
	    || state == NetCameraLegState::WalkStep;
}

constexpr bool BeginsConfirmedCameraKick(bool stateInitialized,
    NetCameraLegState previous, NetCameraLegState current, bool kickLatched)
{
	return IsCameraConfirmedKick(current)
	    && (!stateInitialized || previous != current || !kickLatched);
}

static_assert(IsCameraWalkState(NetCameraLegState::WalkStep));
static_assert(!IsCameraWalkState(NetCameraLegState::KickExtend));
static_assert(IsCameraActionPrelude(NetCameraLegState::Chamber));
static_assert(IsCameraActionPrelude(NetCameraLegState::KneeHold));
static_assert(IsCameraConfirmedKick(NetCameraLegState::KickExtend));
static_assert(IsCameraRecoveryState(NetCameraLegState::Recover));
static_assert(IsCameraGroundState(NetCameraLegState::Grounded));
static_assert(BeginsConfirmedCameraKick(true, NetCameraLegState::Recover,
    NetCameraLegState::KickExtend, true)); // repeated same-side kick
static_assert(BeginsConfirmedCameraKick(true, NetCameraLegState::Grounded,
    NetCameraLegState::KickExtend, false));
static_assert(!BeginsConfirmedCameraKick(true, NetCameraLegState::KickExtend,
    NetCameraLegState::KickExtend, true)); // one held packet is one edge

} // namespace

WalkInPlace& WalkInPlace::Instance()
{
	static WalkInPlace instance;
	return instance;
}

uint64_t WalkInPlace::NowMs()
{
#ifdef OCU_RUNTIME_SELF_TEST
	return testNowMs;
#else
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
#endif
}

#ifdef OCU_RUNTIME_SELF_TEST
WalkInPlace* WalkInPlace::TestCreate()
{
	return new WalkInPlace();
}

void WalkInPlace::TestDestroy(WalkInPlace* instance)
{
	delete instance;
}

void WalkInPlace::TestSetNowMs(uint64_t nowMs)
{
	testNowMs = nowMs;
}

bool WalkInPlace::TestCameraKickLatched(int side) const
{
	return side >= 0 && side < 2 && cameraKickLatched[side];
}
#endif

void WalkInPlace::Update(bool lValid, float lFootY, bool rValid, float rFootY,
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
    bool explicitActivationHeld)
{
	uint64_t now = NowMs();
	const bool trustedHardwareFeet = lHardwareFoot && rHardwareFoot;
	const bool hardwareFoot[2] = { lHardwareFoot, rHardwareFoot };
	float dt = lastUpdateMs ? std::min(0.1f, (now - lastUpdateMs) / 1000.0f) : 0.011f;
	lastUpdateMs = now;
	bool clearFootLift = false;

	// A confirmed camera action invalidates BOTH halves of the cadence proof.
	// Clearing only foot crossings left recent arm reversals alive, so a second
	// kick could combine with the first one's stale arm evidence and relatch
	// forward movement. Keep floor baselines, but erase every temporal decision.
	auto resetCameraGaitEvidence = [&]() {
		for (int i = 0; i < 2; ++i) {
			footUp[i] = false;
			kneeInit[i] = false;
			lastFootLiftMs[i] = 0;
		}
		altDiffInit = false;
		kneeDiffInit = false;
		altState = 0;
		armPhaseInit = false;
		armDiffInit = false;
		armState = 0;
		lastArmPhaseSampleMs = 0;
		armSwingSpeed = 0.0f;
		stepTimes.clear();
		armTimes.clear();
		lastStepMs = 0;
		lastRecordedStepSide = -1;
		lastSwingMs = 0;
		lastArmMotionMs = 0;
		lastClearFootLiftMs = 0;
		lastHighFootLiftMs = 0;
		lastStrongArmMs = 0;
		lastCameraWalkSemanticMs = 0;
		recentPeakLift = 0.0f;
		gaitConfirmed = false;
		gaitReleaseSinceMs = 0;
		quickStartUntilMs = 0;
		runCandidateSinceMs = 0;
		runModeUntilMs = 0;
		outSpeed = 0.0f;
		axisOut.store(0.0f);
		runningOut.store(false);
	};

	bool cameraLegValid[2] = { lCameraLegValid, rCameraLegValid };
	NetCameraLegState cameraLegState[2] = { lCameraLegState, rCameraLegState };
	bool newConfirmedKick = false;
	bool newConfirmedKickSide[2] = { false, false };
	if (!trustedHardwareFeet && cameraSemanticGait) {
		for (int side = 0; side < 2; ++side) {
			if (!hardwareFoot[side] && cameraLegValid[side]
			    && IsCameraWalkState(cameraLegState[side])) {
				lastCameraWalkSemanticMs = now;
				break;
			}
		}
	}
	for (int side = 0; side < 2; ++side) {
		if (hardwareFoot[side]) {
			// A physical side owns itself even in a mixed physical/camera setup.
			cameraKickLatched[side] = false;
			cameraLegStateInit[side] = false;
			cameraGroundedSinceMs[side] = 0;
			continue;
		}
		if (!cameraLegValid[side]) {
				// Never drop a kick latch merely because the ankle left the image.
				// Bound it so stopping the sender cannot permanently disable gait.
				if (cameraKickLatched[side] && cameraKickLastSeenMs[side] != 0
				    && now - cameraKickLastSeenMs[side] > 1200) {
					cameraKickLatched[side] = false;
					cameraGroundedSinceMs[side] = 0;
				}
				cameraLegStateInit[side] = false;
				continue;
		}

		NetCameraLegState state = cameraLegState[side];
		if (state == NetCameraLegState::Invalid) {
			// The sender deliberately publishes Invalid on a confidence
			// dropout. That packet is fresh, but it cannot keep a prior kick
			// latched forever merely by arriving every frame.
			if (cameraKickLatched[side] && cameraKickLastSeenMs[side] != 0
			    && now - cameraKickLastSeenMs[side] > 1200) {
				cameraKickLatched[side] = false;
				cameraGroundedSinceMs[side] = 0;
			}
			cameraLegStateInit[side] = false;
			continue;
		}
		bool beginsKick = BeginsConfirmedCameraKick(cameraLegStateInit[side],
		    lastCameraLegState[side], state, cameraKickLatched[side]);
		if (IsCameraConfirmedKick(state)) {
			if (beginsKick) {
				newConfirmedKick = true;
				newConfirmedKickSide[side] = true;
			}
			cameraKickLatched[side] = true;
			cameraKickLastSeenMs[side] = now;
			cameraGroundedSinceMs[side] = 0;
		} else if (cameraKickLatched[side]) {
			if (IsCameraRecoveryState(state) || IsCameraActionPrelude(state)
			    || state == NetCameraLegState::UndecidedLift) {
				cameraKickLastSeenMs[side] = now;
				cameraGroundedSinceMs[side] = 0;
			} else if (IsCameraGroundState(state)) {
				if (cameraGroundedSinceMs[side] == 0)
					cameraGroundedSinceMs[side] = now;
				else if (now - cameraGroundedSinceMs[side] >= 180) {
					cameraKickLatched[side] = false;
					cameraGroundedSinceMs[side] = 0;
				}
			}
		}

		lastCameraLegState[side] = state;
		cameraLegStateInit[side] = true;
	}

	if (newConfirmedKick) {
		// An established rhythm outvotes a single kick label. A walking stride
		// shares chamber->extend geometry with a strike, so one misclassified
		// stride must not wipe the cadence proof mid-walk. A standing kick
		// still resets normally (no fresh gait), and a genuine kick thrown
		// while walking still resets once the foot climbs above any plausible
		// stride height.
		const bool gaitFresh = gaitConfirmed && lastStepMs != 0
		    && now - lastStepMs <= 900;
		float kickLift = 0.0f;
		for (int side = 0; side < 2; ++side)
			if (newConfirmedKickSide[side])
				kickLift = std::max(kickLift, footFilt[side] - baseline[side]);
		if (!gaitFresh || kickLift >= 0.28f) {
			resetCameraGaitEvidence();
			// The classifier normally supplies Recover for ~450 ms. This tail also
			// covers a missed packet without admitting the return stroke as a step.
			feetRecoveryQuarantineUntilMs = std::max(feetRecoveryQuarantineUntilMs, now + 520);
		} else {
			// Rejected stride-as-kick: drop the latch too, or the action gate
			// would still zero the axis for the rest of the "kick".
			for (int side = 0; side < 2; ++side)
				if (newConfirmedKickSide[side])
					cameraKickLatched[side] = false;
		}
	}
	bool leftAction = !lHardwareFoot && cameraKickLatched[0];
	bool rightAction = !rHardwareFoot && cameraKickLatched[1];
	bool cameraActionNow = leftAction || rightAction;
	bool provisionalAction[2] = {
		!lHardwareFoot && lCameraLegValid
		    && (IsCameraActionPrelude(lCameraLegState)
		        || IsCameraRecoveryState(lCameraLegState)),
		!rHardwareFoot && rCameraLegValid
		    && (IsCameraActionPrelude(rCameraLegState)
		        || IsCameraRecoveryState(rCameraLegState)),
	};
	bool cameraProvisionalActionNow = provisionalAction[0] || provisionalAction[1];
	if (cameraActionNow) {
		feetRecoveryQuarantineUntilMs = std::max(feetRecoveryQuarantineUntilMs, now + 320);
		// Keep the externally visible axis hard-zero throughout extension and
		// recovery, including overlapping alternating kicks.
		gaitConfirmed = false;
		gaitReleaseSinceMs = 0;
		quickStartUntilMs = 0;
		runCandidateSinceMs = 0;
		runModeUntilMs = 0;
		outSpeed = 0.0f;
		axisOut.store(0.0f);
		runningOut.store(false);
	}
	// WalkStep is useful diagnostics, but it is not a second positive gate.
	// The metric feet/knees plus the matching skeleton arms already prove gait.
	// Requiring the classifier to independently rediscover the same cadence
	// caused real six-step/six-swing sequences to remain at axis=0 whenever its
	// conservative lift threshold continued to report Grounded. Camera leg
	// semantics therefore veto actions; they never veto an otherwise complete
	// gait merely because WalkStep was not observed.
	bool cameraWalkSemanticFresh = lastCameraWalkSemanticMs != 0
	    && now - lastCameraWalkSemanticMs <= 1700;
	bool cameraQuickStartAllowed = !cameraSemanticGait;
	if (cameraSemanticGait) {
		bool haveUsableState = false;
		bool semanticVeto = cameraActionNow || cameraProvisionalActionNow;
		for (int side = 0; side < 2; ++side) {
			if (hardwareFoot[side] || !cameraLegValid[side]
			    || cameraLegState[side] == NetCameraLegState::Invalid)
				continue;
			haveUsableState = true;
		}
		cameraQuickStartAllowed = haveUsableState && !semanticVeto;
	}

	// Camera/network tracking can disappear while the Configurator is stopped,
	// the preview changes source, or the person leaves frame. When it comes back
	// its aligned Y origin may be completely different. Relearn the standing
	// floor and phase automatically instead of measuring the new skeleton against
	// a stale baseline. No manual calibration pose is required.
	bool feetSourceRecovered = lValid && rValid && lastFeetSourceMs != 0 && now - lastFeetSourceMs > 150;
	bool armsSourceRecovered = skeletonArmsValid && lastSkeletonSourceMs != 0 && now - lastSkeletonSourceMs > 1000;
	bool commonModeFootRelock = false;
	if (lValid && rValid) {
		if (rawFeetInit) {
			float dl = lFootY - lastRawFootY[0];
			float dr = rFootY - lastRawFootY[1];
			// Both world-space feet cannot teleport together. This is the
			// characteristic camera crop/origin relock seen in the live log;
			// a real kick moves one leg and deliberately does not trip it.
			commonModeFootRelock = dl * dr > 0.0f && std::abs(dl) > 0.08f && std::abs(dr) > 0.08f;
		}
		lastRawFootY[0] = lFootY;
		lastRawFootY[1] = rFootY;
		rawFeetInit = true;
	} else {
		rawFeetInit = false;
	}
	if (lValid && rValid)
		lastFeetSourceMs = now;
	if (skeletonArmsValid)
		lastSkeletonSourceMs = now;
	if (feetSourceRecovered || armsSourceRecovered || commonModeFootRelock) {
		for (int i = 0; i < 2; ++i) {
			baselineInit[i] = false;
			footUp[i] = false;
			kneeInit[i] = false;
		}
		altDiffInit = false;
		kneeDiffInit = false;
		altState = 0;
		armPhaseInit = false;
		armDiffInit = false;
		armState = 0;
		lastArmPhaseSampleMs = 0;
		armSwingSpeed = 0.0f;
		stepTimes.clear();
		armTimes.clear();
		lastStepMs = 0;
		lastFootLiftMs[0] = 0;
		lastFootLiftMs[1] = 0;
		lastRecordedStepSide = -1;
		lastSwingMs = 0;
		lastArmMotionMs = 0;
		lastClearFootLiftMs = 0;
		lastHighFootLiftMs = 0;
		lastStrongArmMs = 0;
		lastCameraWalkSemanticMs = 0;
		recentPeakLift = 0.0f;
		gaitConfirmed = false;
		gaitReleaseSinceMs = 0;
		quickStartUntilMs = 0;
		runCandidateSinceMs = 0;
		runModeUntilMs = 0;
		outSpeed = 0.0f;
		axisOut.store(0.0f);
		if (feetSourceRecovered || commonModeFootRelock)
			feetRecoveryQuarantineUntilMs = now + 900;
	}
	bool gaitInputQuarantined = now < feetRecoveryQuarantineUntilMs;

	// One camera glitch must never move the player. Leg crossings are recorded
	// here, but feet alone no longer confirm locomotion: coordinated skeleton-arm
	// reversals are required by the fusion gate below.
	auto recordLegStep = [&](uint64_t t, int side) {
		if (gaitInputQuarantined)
			return;
		// With both legs available, an L/R phase crossing is only a step if
		// that same foot physically left its floor baseline. This rejects the
		// false opposite crossing caused when one raised leg returns to rest.
		if (side >= 0) {
			// A deliberately slow step can remain airborne longer than the old
			// 1400 ms proof window. If the foot is still up it is direct proof;
			// after touchdown retain the proof for the full slow-cadence range.
			if (!footUp[side] &&
			    (lastFootLiftMs[side] == 0 || t - lastFootLiftMs[side] > 1400))
				return;
			if (lastRecordedStepSide == side)
				return;
		}
		if (lastStepMs != 0) {
			uint64_t gap = t - lastStepMs;
			if (gap < 160)
				return; // impossible double-step / pose bounce
			if (gap > 1700) {
				stepTimes.clear();
				gaitConfirmed = false;
				lastRecordedStepSide = -1;
			}
		}
		stepTimes.push_back(t);
		lastStepMs = t;
		if (side >= 0)
			lastRecordedStepSide = side;
	};

	// ── Per-foot lift tracking. Casual walking reaches us with only 3-5cm
	// of per-foot lift (camera smoothing + scaling eat the rest — the user
	// walks normally, the pipeline shrinks it), so with BOTH feet tracked
	// the step COUNTER is the alternation detector below; this loop keeps
	// the floor estimate honest, provides the instant-start, and counts
	// steps only for single-foot sources.
	// A held physical activation button is explicit intent, so its first-step
	// gate can react to a normal heel lift instead of demanding a march. None
	// keeps the stronger threshold because it is always armed.
	const float liftThresh = explicitActivationHeld ? 0.018f : 0.024f;
	const float dropThresh = explicitActivationHeld ? 0.008f : 0.010f;
	float footY[2] = { lFootY, rFootY };
	// Chamber/KneeHold remain provisional: pause output below, but retain their
	// geometry so a slow/high legitimate stride can still resolve into gait.
	// Only a confirmed kick latch removes a side and clears cadence evidence.
	bool footValid[2] = { lValid && !leftAction, rValid && !rightAction };
	bool bothFeet = footValid[0] && footValid[1];
	for (int f = 0; f < 2; f++) {
		if (!footValid[f])
			continue;
		if (!baselineInit[f]) {
			footFilt[f] = footY[f];
			baseline[f] = footY[f];
			baselineInit[f] = true;
			continue;
		}
		// Spike blunting: a real foot moves ≤ ~3 m/s vertically. One-frame
		// camera glitches (±0.3m ankle teleports seen live) otherwise
		// poison the floor estimate and fake/eat steps.
		footFilt[f] += std::clamp(footY[f] - footFilt[f], -0.035f, 0.035f);
		float fy = footFilt[f];

		// Baseline = the STANDING level. Tracks DOWN quickly but at a
		// bounded rate (instant min-snap adopted glitch minima as floor),
		// creeps up slowly while the foot is down (dt-based, ~1.7s
		// half-life — a per-frame constant chased the foot at 90Hz).
		if (fy < baseline[f])
			baseline[f] = std::max(fy, baseline[f] - 0.006f);
		else if (!footUp[f])
			baseline[f] += (dt / 2.5f) * (fy - baseline[f]);
		if (fy - baseline[f] >= 0.14f)
			lastHighFootLiftMs = now;

		if (!footUp[f] && fy > baseline[f] + liftThresh) {
			footUp[f] = true;
			footUpAt[f] = now;
			lastFootLiftMs[f] = now;
			minSinceUp[f] = fy;
			clearFootLift = true;
		} else if (footUp[f]) {
			minSinceUp[f] = std::min(minSinceUp[f], fy);
			if (fy < baseline[f] + dropThresh) {
				footUp[f] = false;
				if (!bothFeet)
					recordLegStep(now, -1);
			} else if (now - footUpAt[f] > 1500) {
				// The baseline is stale — re-adopt the LOWEST height seen
				// during the stuck stretch, never the current mid-air one.
				footUp[f] = false;
				baseline[f] = minSinceUp[f];
			}
		}
	}
	if (clearFootLift)
		lastClearFootLiftMs = now;
	if (gaitInputQuarantined) {
		clearFootLift = false;
		lastClearFootLiftMs = 0;
		lastHighFootLiftMs = 0;
		quickStartUntilMs = 0;
	}

	// A selected, held activation button is explicit walking intent. One clear
	// lift may therefore start a gentle preview immediately while the stricter
	// feet+arms cadence gate continues confirming the full gait in parallel.
	// With activation=None this path is never armed, preserving the stronger
	// anti-false-positive gate for always-on walking.
	if (explicitActivationHeld && clearFootLift && cameraQuickStartAllowed)
		quickStartUntilMs = now + 850;
	else if (!explicitActivationHeld || !cameraQuickStartAllowed)
		quickStartUntilMs = 0;
	bool quickStartActive = explicitActivationHeld && cameraQuickStartAllowed
	    && now < quickStartUntilMs;

	// ── Leg-alternation step counter. Walking strictly alternates the
	// legs: the L-R height differences of ankles AND knees swing
	// anti-phase (double the per-joint amplitude) while camera noise
	// common to both sides cancels out. Ankles and knees are FUSED into
	// one signal — "the math is the gate": every tracked joint pair votes
	// on the same rhythm, so a casual walk that any single joint would
	// miss is unmistakable in the sum. Each centered signal absorbs its
	// static offset via a slow mean; each hysteresis crossing = one
	// footfall.
	{
		float kneeY[2] = { lKneeY, rKneeY };
		bool kneeValid[2] = { lkValid && !leftAction, rkValid && !rightAction };
		for (int k = 0; k < 2; k++) {
			if (!kneeValid[k])
				continue;
			if (!kneeInit[k]) {
				kneeFilt[k] = kneeY[k];
				kneeInit[k] = true;
			}
			kneeFilt[k] += std::clamp(kneeY[k] - kneeFilt[k], -0.035f, 0.035f);
		}
		// Match the semantic action mask used while filtering above.  Using the
		// raw validity flags here reused a frozen knee sample during a kick and
		// could leave an artificial phase crossing queued for after quarantine.
		bool bothKnees = kneeValid[0] && kneeValid[1];

		float dc = 0.0f;
		int sources = 0;
		if (bothFeet) {
			float d = footFilt[0] - footFilt[1];
			if (!altDiffInit) {
				altDiffSlow = d;
				altDiffInit = true;
			}
			altDiffSlow += (dt / 2.0f) * (d - altDiffSlow);
			dc += d - altDiffSlow;
			sources++;
		}
		if (bothKnees) {
			float d = kneeFilt[0] - kneeFilt[1];
			if (!kneeDiffInit) {
				kneeDiffSlow = d;
				kneeDiffInit = true;
			}
			kneeDiffSlow += (dt / 2.0f) * (d - kneeDiffSlow);
			dc += 0.7f * (d - kneeDiffSlow);
			sources++;
		}
		if (sources > 0) {
			// More agreeing joints = more signal, so the bar scales up
			// with sources while the signal grows faster than it.
			float h = sources == 2 ? 0.028f : 0.020f;
			int ns = altState;
			if (dc > h)
				ns = 1;
			else if (dc < -h)
				ns = -1;
			if (ns != altState) {
				if (altState != 0)
					recordLegStep(now, ns > 0 ? 0 : 1);
				altState = ns;
			}
		}
	}

	// ── Skeleton arm-swing corroborator.
	// Camera-skeleton shoulders/elbows/wrists produce the signed L-R phase.
	// Controller position is deliberately absent: Meta optical loss beneath
	// the torso must not suppress or invent walking. Feet still own the final
	// decision, so isolated skeleton arm movement cannot move the player.
	if (skeletonArmsValid && !gaitInputQuarantined) {
		// The camera sender runs at inference cadence (about 7-15 Hz in the
		// live capture), while this function runs at the VR frame rate. Its OSC
		// phase value is therefore repeated unchanged for many Update calls.
		// Dividing the next phase jump by the 90 Hz frame dt inflated ordinary
		// arm motion into a run spike. Measure only distinct sender samples and
		// divide by the elapsed sample time instead.
		bool newArmSample = false;
		float phaseSpeed = armSwingSpeed;
		if (!armPhaseInit) {
			previousArmPhase = skeletonArmPhase;
			armPhaseInit = true;
			lastArmPhaseSampleMs = now;
		} else if (std::abs(skeletonArmPhase - previousArmPhase) > 0.00001f) {
			uint64_t sampleElapsedMs = lastArmPhaseSampleMs ? now - lastArmPhaseSampleMs : 0;
			float sampleDt = sampleElapsedMs / 1000.0f;
			if (sampleElapsedMs >= 20 && sampleElapsedMs <= 600) {
				phaseSpeed = std::clamp(
				    std::abs(skeletonArmPhase - previousArmPhase) / std::max(sampleDt, 0.001f),
				    0.0f, 4.0f);
				const float armTau = phaseSpeed > armSwingSpeed ? 0.10f : 0.30f;
				armSwingSpeed += (1.0f - std::exp(-sampleDt / armTau)) * (phaseSpeed - armSwingSpeed);
				newArmSample = true;
				previousArmPhase = skeletonArmPhase;
				lastArmPhaseSampleMs = now;
			} else if (sampleElapsedMs > 600) {
				// A first packet after a long held/stale value is a re-lock, not
				// evidence of a several-metres-per-second arm swing.
				armSwingSpeed = 0.0f;
				previousArmPhase = skeletonArmPhase;
				lastArmPhaseSampleMs = now;
			}
			// Do not consume a changed phase sample that arrived in under 20 ms.
			// A fast (~60 Hz) sender would otherwise advance this reference on
			// every frame without ever producing one accepted velocity sample.
			// Leaving the reference untouched accumulates a real >=20 ms window.
		} else if (lastArmPhaseSampleMs != 0 && now - lastArmPhaseSampleMs > 300) {
			// Hold velocity between normal camera frames. Decay only when the
			// sender has actually stopped changing for longer than two frames.
			armSwingSpeed += (1.0f - std::exp(-dt / 0.30f)) * (0.0f - armSwingSpeed);
		}
		if (newArmSample && armSwingSpeed >= 0.07f)
			lastArmMotionMs = now;
		if (newArmSample && armSwingSpeed >= 0.60f)
			lastStrongArmMs = now;

		if (!armDiffInit) {
			armDiffSlow = skeletonArmPhase;
			armDiffInit = true;
		}
		armDiffSlow += (dt / 2.5f) * (skeletonArmPhase - armDiffSlow);
		float dc = skeletonArmPhase - armDiffSlow;
		const float h = 0.035f;
		int ns = armState;
		if (dc > h)
			ns = 1;
		else if (dc < -h)
			ns = -1;
		if (newArmSample && ns != armState) {
			if (armState != 0 && armSwingSpeed >= 0.07f) {
				if (lastSwingMs == 0 || now - lastSwingMs >= 160) {
					armTimes.push_back(now);
					lastSwingMs = now;
				}
			}
			armState = ns;
		}
	} else {
		armPhaseInit = false;
		lastArmPhaseSampleMs = 0;
		armDiffInit = false;
		armState = 0;
		armSwingSpeed += (1.0f - std::exp(-dt / 0.18f)) * (0.0f - armSwingSpeed);
	}

	auto prune = [&](std::vector<uint64_t>& v) {
		v.erase(std::remove_if(v.begin(), v.end(),
		            // Three legal 1.3 s slow steps span 2.6 s. The old 2.2 s
		            // horizon deleted step one before step three could confirm.
		            [&](uint64_t t) { return now - t > 3200; }),
		    v.end());
		if (v.size() > 6)
			v.erase(v.begin(), v.end() - 6);
	};
	prune(stepTimes);
	prune(armTimes);

	// ── Cadence fusion: interval-based (mean gap), NOT a count-in-window
	// (window counts quantize and made the speed surge). Feet own the
	// decision to move at all; the arm stream refines/sustains cadence
	// when it is fresher than the feet (camera missed a step but the arms
	// kept swinging).
	size_t nf = stepTimes.size(), na = armTimes.size();
	auto recentMeanGap = [](const std::vector<uint64_t>& events) {
		if (events.size() < 2)
			return 0.0f;
		size_t first = events.size() > 4 ? events.size() - 4 : 0;
		return (float)(events.back() - events[first]) / (events.size() - 1 - first);
	};
	float footGap = recentMeanGap(stepTimes);
	float armGap = recentMeanGap(armTimes);
	bool cadenceMatches = false;
	int pairedCrossings = 0;
	if (footGap > 0.0f && armGap > 0.0f) {
		float ratio = footGap / armGap;
		cadenceMatches = ratio >= 0.60f && ratio <= 1.67f;
		uint64_t pairWindow = (uint64_t)std::clamp(0.45f * footGap, 170.0f, 300.0f);
		for (uint64_t ft : stepTimes) {
			bool paired = false;
			for (uint64_t at : armTimes) {
				uint64_t delta = ft > at ? ft - at : at - ft;
				if (delta <= pairWindow) {
					paired = true;
					break;
				}
			}
			if (paired)
				++pairedCrossings;
		}
	}

	bool strictCoordinated = nf >= 2 && na >= 2 && cadenceMatches && pairedCrossings >= 2 &&
	    now - lastStepMs < 900 && now - lastSwingMs < 900 && armSwingSpeed >= 0.10f;
	// A natural slow walk produces obvious alternating knees/ankles but its arm
	// phase may never cross the centered line twice. Three alternating leg
	// crossings plus an actual foot lift and recent arm motion is still a complete
	// skeleton rhythm; accept it at a capped walking speed without weakening the
	// faster two-stream cadence path above.
	bool trustedFeetSlowEvidence = trustedHardwareFeet;
	bool cameraSlowEvidence = skeletonArmsValid && lastArmMotionMs != 0 && now - lastArmMotionMs < 1500;
	bool slowCadence = nf >= 3 && footGap >= 300.0f && footGap <= 1300.0f &&
	    lastStepMs != 0 && now - lastStepMs < 1500 &&
	    lastClearFootLiftMs != 0 && now - lastClearFootLiftMs < 1500 &&
	    (trustedFeetSlowEvidence || cameraSlowEvidence);
	// Running is equally obvious even if a single arm/leg crossing misses the
	// narrow pairing window: at least four recent alternating leg crossings plus
	// high skeleton-arm velocity. This preserves a run through camera occlusion
	// without allowing arm flailing alone to move the player.
	bool recentHighLift = lastHighFootLiftMs != 0 && now - lastHighFootLiftMs < 900;
	bool cameraRunEvidence = skeletonArmsValid && lastStrongArmMs != 0 &&
	    now - lastStrongArmMs < 900 && recentHighLift;
	size_t freshCameraRunSteps = (size_t)std::count_if(stepTimes.begin(), stepTimes.end(),
	    [&](uint64_t t) { return now - t <= 1550; });
	// A low-frame-rate camera merges crossings and stretches the measured gap,
	// so the run gate leans on the high-lift + strong-arm evidence (which
	// survives low fps) rather than demanding sprint-tight measured cadence.
	bool cameraRunCandidate = freshCameraRunSteps >= 3 && footGap >= 160.0f && footGap <= 700.0f &&
	    lastStepMs != 0 && now - lastStepMs < 850 && cameraRunEvidence;
	bool hardwareRunCandidate = trustedHardwareFeet && nf >= 4 &&
	    footGap >= 160.0f && footGap <= 650.0f &&
	    lastStepMs != 0 && now - lastStepMs < 700;
	bool runCandidate = cameraRunCandidate || hardwareRunCandidate;
	if (runCandidate) {
		if (runCandidateSinceMs == 0)
			runCandidateSinceMs = now;
	} else {
		runCandidateSinceMs = 0;
	}
	bool runCadence = runCandidateSinceMs != 0 && now - runCandidateSinceMs >= 240;
	// World3D geometry supplies cadence and speed. Semantic action states are a
	// negative safety gate only: Chamber/KneeHold/Recover pause and KickExtend
	// clears cadence. A Grounded state must not suppress independently proven
	// alternating feet + arms.
	bool coordinatedNow = !cameraActionNow && !cameraProvisionalActionNow
	    && (strictCoordinated || slowCadence || runCadence);
	if (coordinatedNow) {
		gaitConfirmed = true;
		gaitReleaseSinceMs = 0;
	}
	if (runCadence)
		runModeUntilMs = now + 1100;

	float expectedGap = footGap > 0.0f ? footGap : 500.0f;
	// A 500 ms lower bound was shorter than an ordinary slow half-step and made
	// the latch chatter 1/0/1 while the user was still walking. Give one missed
	// camera crossing room, then let the speed smoother release naturally.
	uint64_t stopTimeout = (uint64_t)std::clamp(2.6f * expectedGap, 850.0f, 1700.0f);
	uint64_t lastArmEvidenceMs = std::max(lastSwingMs, lastArmMotionMs);
	bool corroborationExpired = !trustedHardwareFeet &&
	    (lastArmEvidenceMs == 0 || now - lastArmEvidenceMs > stopTimeout);
	uint64_t lastLegEvidenceMs = std::max(lastStepMs, lastClearFootLiftMs);
	bool legEvidenceExpired = lastLegEvidenceMs == 0 || now - lastLegEvidenceMs > stopTimeout;
	bool releaseEvidenceExpired = trustedHardwareFeet ? legEvidenceExpired
	                                                  : (legEvidenceExpired || corroborationExpired);
	if (gaitConfirmed && releaseEvidenceExpired) {
		if (gaitReleaseSinceMs == 0)
			gaitReleaseSinceMs = now;
		else if (now - gaitReleaseSinceMs >= 300)
			gaitConfirmed = false;
	} else {
		gaitReleaseSinceMs = 0;
	}
	bool runningNow = gaitConfirmed && now < runModeUntilMs
	    && !cameraProvisionalActionNow;
	runningOut.store(runningNow);

	// Feet own ordinary walking speed. Arms remain the camera-gait corroborator
	// above, but changing between the strict arm-crossing path and the slow-foot
	// fallback must not select two different speed curves. That branch switch
	// caused a steady ~650 ms gait to jump between roughly 0.18 and 0.49.
	// Lift amplitude survives a low-frame-rate camera far better than crossing
	// cadence, which quantizes toward whole frames and collapses a shuffle and
	// a sprint into similar measured gaps. Blend amplitude into the speed so
	// gait intensity, not just cadence, sets locomotion power: the slowest
	// shuffle creeps like a barely-tilted stick and ramps continuously.
	{
		float liftNow = std::max(footFilt[0] - baseline[0], footFilt[1] - baseline[1]);
		recentPeakLift = std::max(recentPeakLift * std::exp(-dt / 1.5f),
		    std::clamp(liftNow, 0.0f, 0.60f));
	}
	float liftIntent = std::clamp((recentPeakLift - 0.02f) / (0.10f - 0.02f), 0.0f, 1.0f);
	float targetSpeed = 0.0f;
	if (gaitConfirmed && footGap > 0.0f && runningNow) {
		float footSps = 1000.0f / std::max(footGap, 1.0f);
		float cadenceIntent = std::clamp((footSps - 1.8f) / (5.0f - 1.8f), 0.45f, 1.0f);
		if (trustedHardwareFeet) {
			targetSpeed = cadenceIntent;
		} else {
			// Camera cadence under-measures a real sprint; a confirmed run's
			// floor must already feel like running, with arm speed or lift
			// intensity carrying it to full sprint.
			float armIntent = std::clamp((armSwingSpeed - 0.45f) / (2.0f - 0.45f), 0.0f, 1.0f);
			targetSpeed = std::clamp(0.75f + 0.25f * std::max(armIntent, liftIntent),
			    0.75f, 1.0f);
		}
	} else if (gaitConfirmed && footGap > 0.0f) {
		float footSps = 1000.0f / std::max(footGap, 1.0f);
		float cadenceNorm = std::clamp((footSps - 0.75f) / (3.20f - 0.75f), 0.0f, 1.0f);
		// Continuous walk band from creep to brisk: amplitude leads because a
		// slow camera reports similar cadence for very different efforts.
		targetSpeed = std::clamp(
		    0.14f + 0.50f * (0.45f * cadenceNorm + 0.55f * liftIntent),
		    0.14f, 0.62f);
	} else if (quickStartActive) {
		targetSpeed = 0.15f;
	}
	if (cameraProvisionalActionNow)
		targetSpeed = 0.0f;

	// Smooth speed, framerate-independent. No-button walking deliberately keeps
	// its three-step acquisition gate; after it confirms, speed attacks without
	// a jolt and releases more gently across one missed camera crossing.
	float tau = targetSpeed > outSpeed ? (quickStartActive ? 0.10f : 0.22f)
	                                      : (gaitConfirmed ? 0.55f : 0.45f);
	outSpeed += (1.0f - std::exp(-dt / tau)) * (targetSpeed - outSpeed);
	if (outSpeed < 0.03f && targetSpeed == 0.0f)
		outSpeed = 0.0f;

	// ── Direction gesture (user-designed): BOTH hands held at roughly chin
	// height = walk backwards. Prefer camera-skeleton hands; tracker-only users
	// fall back to controller grip height. Controllers never drive gait cadence,
	// and the high/chin pose is inside the headset's reliable tracking volume.
	// Both engage AND release need
	// sustain: instant engage latched on forward-run arm pumps (one frame
	// past the line flipped it), and his backwards style pumps the arms,
	// so release needs the hands clearly down for a sustained moment.
	int directionHandSource = 0; // 0=none, 1=camera skeleton, 2=controllers
	float directionLeftY = 0.0f;
	float directionRightY = 0.0f;
	if (skeletonArmsValid) {
		directionHandSource = 1;
		directionLeftY = lSkeletonHandY;
		directionRightY = rSkeletonHandY;
	} else if (lTurnCtrlValid && rTurnCtrlValid) {
		directionHandSource = 2;
		directionLeftY = lCtrlY;
		directionRightY = rCtrlY;
	}
	if (hmdValid && directionHandSource != 0) {
		float raiseLine = hmdY - 0.35f; // ~chin/upper-chest
		float dropLine = hmdY - 0.50f; // clearly below the gesture
		bool bothHigh = directionLeftY > raiseLine && directionRightY > raiseLine;
		bool eitherLow = directionLeftY < dropLine || directionRightY < dropLine;

		if (!goingBackward) {
			if (bothHigh) {
				if (handsHighSince == 0)
					handsHighSince = now;
				else if (now - handsHighSince > 350) {
					goingBackward = true;
					handsBelowSince = 0;
				}
			} else {
				handsHighSince = 0;
			}
		} else {
			handsHighSince = 0;
			if (eitherLow) {
				if (handsBelowSince == 0)
					handsBelowSince = now;
				else if (now - handsBelowSince > 400)
					goingBackward = false;
			} else {
				handsBelowSince = 0;
			}
		}
	} else {
		goingBackward = false; // no usable hands: fail safe to forward
		handsHighSince = 0;
		handsBelowSince = 0;
	}

#ifdef OCU_RUNTIME_SELF_TEST
	const float scale = 1.0f;
#else
	float scale = oovr_global_configuration.WalkInPlaceSpeed();
#endif
	float v = cameraProvisionalActionNow
	    ? 0.0f
	    : outSpeed * (goingBackward ? -1.0f : 1.0f) * scale;
	axisOut.store(std::clamp(v, -1.0f, 1.0f));

	// Outward controller-head steering. OpenXR aim local -Z is the standardized
	// controller pointing ray. Rotate the right controller until that axis
	// points right (palm toward the camera) to turn right; mirror it on the left.
	// Unlike the old absolute palm test, rotating a hand inward cannot trigger
	// the turn. A short dwell and hysteresis reject ordinary arm-pump tilts.
	float turnPoseScore[2] = {};
	float turnPalmScore[2] = {};
	float targetTurn = 0.0f;
	float headLen = std::sqrt(hmdForwardX * hmdForwardX + hmdForwardZ * hmdForwardZ);
	bool locomotionActive = !cameraProvisionalActionNow
	    && (gaitConfirmed || quickStartActive || outSpeed > 0.05f);
	if (locomotionActive && hmdValid && (lTurnCtrlValid || rTurnCtrlValid) && headLen > 0.001f) {
		float ctrlHeadX[2] = { lCtrlHeadX, rCtrlHeadX };
		float ctrlHeadZ[2] = { lCtrlHeadZ, rCtrlHeadZ };
		float palmFrontX[2] = { lPalmFrontX, rPalmFrontX };
		float palmFrontZ[2] = { lPalmFrontZ, rPalmFrontZ };
		bool ctrlValid[2] = { lTurnCtrlValid, rTurnCtrlValid };
		float hmdRightX = -hmdForwardZ / headLen;
		float hmdRightZ = hmdForwardX / headLen;
		for (int hand = 0; hand < 2; ++hand) {
			if (!ctrlValid[hand]) {
				turnGesture[hand] = false;
				turnGestureSince[hand] = 0;
				continue;
			}
			float poseLen = std::sqrt(ctrlHeadX[hand] * ctrlHeadX[hand] + ctrlHeadZ[hand] * ctrlHeadZ[hand]);
			float palmLen = std::sqrt(palmFrontX[hand] * palmFrontX[hand] + palmFrontZ[hand] * palmFrontZ[hand]);
			float outwardSign = hand == 0 ? -1.0f : 1.0f;
			if (poseLen > 0.001f)
				turnPoseScore[hand] = outwardSign *
				    (ctrlHeadX[hand] * hmdRightX + ctrlHeadZ[hand] * hmdRightZ) / poseLen;
			if (palmLen > 0.001f)
				turnPalmScore[hand] =
				    (palmFrontX[hand] * hmdForwardX + palmFrontZ[hand] * hmdForwardZ) /
				    (palmLen * headLen);

			if (!turnGesture[hand]) {
				if (turnPoseScore[hand] >= 0.78f && turnPalmScore[hand] >= 0.60f) {
					if (turnGestureSince[hand] == 0)
						turnGestureSince[hand] = now;
					else if (now - turnGestureSince[hand] >= 220)
						turnGesture[hand] = true;
				} else {
					turnGestureSince[hand] = 0;
				}
			} else if (turnPoseScore[hand] < 0.58f || turnPalmScore[hand] < 0.42f) {
				turnGesture[hand] = false;
				turnGestureSince[hand] = 0;
			}
		}

		float leftIntent = turnGesture[0] ? std::clamp((turnPoseScore[0] - 0.55f) / 0.35f, 0.0f, 1.0f) : 0.0f;
		float rightIntent = turnGesture[1] ? std::clamp((turnPoseScore[1] - 0.55f) / 0.35f, 0.0f, 1.0f) : 0.0f;
		targetTurn = 0.85f * (rightIntent - leftIntent);
	} else {
		turnGesture[0] = turnGesture[1] = false;
		turnGestureSince[0] = turnGestureSince[1] = 0;
	}

	float turnTau = std::abs(targetTurn) > std::abs(turnOut) ? 0.10f : 0.18f;
	turnOut += (1.0f - std::exp(-dt / turnTau)) * (targetTurn - turnOut);
	if (cameraProvisionalActionNow)
		turnOut = 0.0f;
	if (std::abs(turnOut) < 0.02f && targetTurn == 0.0f)
		turnOut = 0.0f;
	turnOutAtomic.store(std::clamp(turnOut, -1.0f, 1.0f));

	// Diagnostic heartbeat (1/s): every link of the chain in one line
	static uint64_t lastLog = 0;
	if (oovr_debug_logging_enabled() && now - lastLog > 1000) {
		lastLog = now;
		OOVR_LOGF("WIP: L(%d y=%.3f base=%.3f) R(%d y=%.3f base=%.3f) knees=(%d %.3f,%d %.3f) hwFeet=%d semMode=%d semWalk=%d sem=(%d:%d,%d:%d) action=%d steps=%d arms=%d paired=%d gaps=(%.0f,%.0f) skel=%d q=%d dirHands=%d armPhase=%.3f armV=%.2f gait=%d slow=%d run=%d quick=%d speed=%.2f back=%d axis=%.2f turnCtrl=(%d,%d) turnPose=(%.2f,%.2f) turnPalm=(%.2f,%.2f) turn=%.2f",
		    lValid ? 1 : 0, footFilt[0], baseline[0], rValid ? 1 : 0, footFilt[1], baseline[1],
		    lkValid ? 1 : 0, lKneeY, rkValid ? 1 : 0, rKneeY,
		    trustedHardwareFeet ? 1 : 0,
		    cameraSemanticGait ? 1 : 0, cameraWalkSemanticFresh ? 1 : 0,
		    lCameraLegValid ? 1 : 0, static_cast<int>(lCameraLegState),
		    rCameraLegValid ? 1 : 0, static_cast<int>(rCameraLegState), cameraActionNow ? 1 : 0,
		    (int)stepTimes.size(), (int)armTimes.size(), pairedCrossings, footGap, armGap,
		    skeletonArmsValid ? 1 : 0, gaitInputQuarantined ? 1 : 0, directionHandSource, skeletonArmPhase, armSwingSpeed,
		    gaitConfirmed ? 1 : 0, slowCadence ? 1 : 0, runningNow ? 1 : 0, quickStartActive ? 1 : 0, outSpeed, goingBackward ? 1 : 0, axisOut.load(),
		    lTurnCtrlValid ? 1 : 0, rTurnCtrlValid ? 1 : 0,
		    turnPoseScore[0], turnPoseScore[1], turnPalmScore[0], turnPalmScore[1], turnOutAtomic.load());
	}
}
