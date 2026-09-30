#include "OpenOVR/Misc/NetworkTrackers.h"
#include "OpenOVR/Misc/WalkInPlace.h"

#include <cmath>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message)
{
	if (condition)
		return;
	std::cerr << "FAIL: " << message << '\n';
	++failures;
}

bool Near(float actual, float expected, float epsilon = 0.0001f)
{
	return std::abs(actual - expected) <= epsilon;
}

void TestRawMediaPipeNativePath()
{
	constexpr int headerBytes = 72;
	constexpr int landmarkBytes = 16;
	std::array<char, headerBytes + 33 * landmarkBytes> packet{};
	packet[0] = 'O'; packet[1] = 'C'; packet[2] = 'U'; packet[3] = '3';
	auto u32 = [&](int offset, uint32_t value) {
		packet[offset] = static_cast<char>(value);
		packet[offset + 1] = static_cast<char>(value >> 8);
		packet[offset + 2] = static_cast<char>(value >> 16);
		packet[offset + 3] = static_cast<char>(value >> 24);
	};
	auto u64 = [&](int offset, uint64_t value) {
		u32(offset, static_cast<uint32_t>(value));
		u32(offset + 4, static_cast<uint32_t>(value >> 32));
	};
	auto f32 = [&](int offset, float value) {
		uint32_t bits = 0;
		memcpy(&bits, &value, sizeof(bits));
		u32(offset, bits);
	};
	u32(4, 1);
	u32(8, 700);
	u32(12, 1u << 1); // arms valid; input is not mirrored
	u64(16, 1);
	u64(24, 1000);
	u32(32, static_cast<uint32_t>(NetCameraLegState::Grounded));
	u32(36, static_cast<uint32_t>(NetCameraLegState::Grounded));
	f32(40, 175.0f); f32(44, 0.95f);
	f32(48, 174.0f); f32(52, 0.94f);
	f32(56, 0.12f); f32(60, 1.10f); f32(64, 1.08f);
	for (int i = 0; i < 33; ++i)
		f32(headerBytes + i * landmarkBytes + 12, -1.0f);
	auto landmark = [&](int index, float userRightX, float userUpY,
	                    float userForwardZ, float confidence = 0.99f) {
		const int offset = headerBytes + index * landmarkBytes;
		// This is the real untouched MediaPipe layout for a person facing an
		// unmirrored camera: anatomical left is positive raw X, up is negative
		// raw Y, and closer to the camera is negative raw Z.
		f32(offset, -userRightX);
		f32(offset + 4, -userUpY);
		f32(offset + 8, -userForwardZ);
		f32(offset + 12, confidence);
	};
	landmark(11, -0.22f, 1.45f, 0.0f); // shoulders
	landmark(12, 0.22f, 1.45f, 0.0f);
	landmark(13, -0.35f, 1.10f, 0.0f); // elbows
	landmark(14, 0.35f, 1.10f, 0.0f);
	landmark(23, -0.15f, 1.00f, 0.0f); // hips
	landmark(24, 0.15f, 1.00f, 0.0f);
	landmark(25, -0.20f, 0.52f, 0.0f); // knees
	landmark(26, 0.20f, 0.52f, 0.0f);
	landmark(27, -0.32f, 0.05f, 0.24f); // left ankle kicked toward camera
	landmark(28, 0.32f, 0.05f, 0.0f);
	landmark(29, -0.32f, 0.00f, 0.20f); // heels
	landmark(30, 0.32f, 0.00f, -0.04f);
	landmark(31, -0.32f, 0.00f, 0.40f); // toes
	landmark(32, 0.32f, 0.00f, 0.16f);

	auto& receiver = NetworkTrackerReceiver::Instance();
	receiver.TestReset();
	receiver.TestParseRawMediaPipePacket(packet.data(), static_cast<int>(packet.size()));
	NetTrackerFrameSample frame;
	NetTrackerSample waist, leftFoot, rightFoot, head;
	Check(receiver.GetTrackerFrame(frame, 1000), "raw MediaPipe frame commits atomically");
	Check((frame.flags & NetTrackerFrame_Continuous3D) != 0
	        && (frame.flags & NetTrackerFrame_HeadXZIsBodyRoot) != 0,
	    "raw path advertises continuous body-root alignment");
	Check((frame.flags & NetTrackerFrame_FollowHmdYaw) == 0,
	    "raw camera body never follows independent HMD yaw");
	Check(receiver.GetTracker(0, waist) && receiver.GetTracker(1, leftFoot)
	        && receiver.GetTracker(2, rightFoot) && receiver.GetHead(head),
	    "native raw path publishes waist, both feet and head anchor");
	Check(Near(waist.pos[0], 0.0f) && Near(waist.pos[1], 1.0f),
	    "native waist uses the hip centre without C# coordinate correction");
	Check(Near(leftFoot.pos[0], -0.32f) && Near(rightFoot.pos[0], 0.32f),
	    "anatomical left/right foot spread survives the native mapping");
	Check(Near(leftFoot.pos[2], 0.24f) && Near(rightFoot.pos[2], 0.0f),
	    "a front-facing camera kick maps toward avatar forward, never behind");
	Check(Near(leftFoot.pos[1], 0.05f) && Near(rightFoot.pos[1], 0.05f),
	    "both feet share one floor-relative transform");
	Check(Near(head.pos[0], waist.pos[0]) && Near(head.pos[2], waist.pos[2]),
	    "synthetic head X/Z is exactly the pelvis root");

	float hmdPosition[3] = { 0.0f, head.pos[1], 0.0f };
	float forwardAtCalibration[2] = { 0.0f, -1.0f };
	receiver.UpdateAlignment(hmdPosition, forwardAtCalibration, nullptr, nullptr);
	const float lockedYaw = receiver.GetAlignmentYaw();
	Check(Near(lockedYaw, 0.0f),
	    "front-facing anatomical landmarks do not create a false 180-degree yaw");
	float headTurnedRight[2] = { 1.0f, 0.0f };
	receiver.UpdateAlignment(hmdPosition, headTurnedRight, nullptr, nullptr);
	Check(Near(receiver.GetAlignmentYaw(), lockedYaw),
	    "turning only the HMD cannot rotate the camera waist or legs");

	// The camera sees a straight raised right-leg kick but deliberately sends a
	// stale Grounded semantic. Native geometry must promote both sides through
	// the same rule because FBT receives no separate knee target.
	u64(16, 2);
	u64(24, 1016);
	u32(36, static_cast<uint32_t>(NetCameraLegState::Grounded));
	// Match the captured held-right-leg failure: the grounded frame measured a
	// roughly 0.95 m leg, but raised monocular joints collapse it to about 0.60 m
	// while remaining collinear and choosing the backward depth solution.
	landmark(26, 0.18f, 0.76f, -0.18f);
	landmark(28, 0.20f, 0.52f, -0.36f);
	receiver.TestParseRawMediaPipePacket(packet.data(), static_cast<int>(packet.size()));
	NetCameraLegSample rightLeg;
	Check(receiver.GetCameraLeg(1, rightLeg, 1000)
	        && rightLeg.state == NetCameraLegState::KickExtend,
	    "captured low straight-leg hold promotes to kick extension");
	Check(receiver.GetTracker(2, rightFoot) && rightFoot.pos[2] > 0.30f,
	    "captured monocular backward-depth kick is reflected avatar-forward");
	const float rightReach = std::sqrt(
	    (rightFoot.pos[0] - 0.15f) * (rightFoot.pos[0] - 0.15f)
	    + (rightFoot.pos[1] - 1.00f) * (rightFoot.pos[1] - 1.00f)
	    + rightFoot.pos[2] * rightFoot.pos[2]);
	Check(rightReach > 0.85f,
	    "raised foreshortened kick retains grounded standing leg reach");
}

void TestAtomicGaitCommitAndOmission()
{
	auto& receiver = NetworkTrackerReceiver::Instance();
	receiver.TestReset();
	const uint32_t flags = NetTrackerFrame_Continuous3D
	    | NetTrackerFrame_AllowGaitFootRelease;

	// Enter the framed contract. Subsequent payloads must remain staged until
	// the frame trailer commits one coherent generation.
	receiver.TestCommitFrame(100, 0, flags);

	NetTrackerFrameSample initialFrame;
	Check(receiver.GetTrackerFrame(initialFrame, 1000),
	    "initial frame is visible");
	Check(initialFrame.generation == 1, "first commit has generation 1");

	receiver.TestStoreTrackerPosition(0, 1.0f, 2.0f, 3.0f);
	receiver.TestStoreSkeletonArms(0.25f, 1.1f, 1.2f);
	receiver.TestStoreCameraLeg(0, NetCameraLegState::WalkStep, 152.0f, 0.91f);
	receiver.TestStoreCameraLeg(1, NetCameraLegState::KickExtend, 169.0f, 0.94f);

	NetTrackerSample tracker;
	NetSkeletonArmsSample arms;
	NetCameraLegSample leg;
	Check(!receiver.GetTracker(0, tracker),
	    "tracker payload is not exposed before frame commit");
	Check(!receiver.GetSkeletonArms(arms),
	    "arm semantics are not exposed before frame commit");
	Check(!receiver.GetCameraLeg(0, leg, 1000),
	    "leg semantics are not exposed before frame commit");

	receiver.TestCommitFrame(100, 1u << 0, flags);
	NetTrackerFrameSample committedFrame;
	Check(receiver.GetTrackerFrame(committedFrame, 1000),
	    "committed frame remains fresh");
	Check(committedFrame.generation == 2,
	    "second frame advances generation exactly once");
	Check(receiver.GetTracker(0, tracker), "tracker commits with frame");
	Check(Near(tracker.pos[0], 1.0f) && Near(tracker.pos[1], 2.0f)
	        && Near(tracker.pos[2], 3.0f),
	    "committed tracker coordinates are coherent");
	Check(receiver.GetSkeletonArms(arms), "arms commit with frame");
	Check(arms.sourceEpoch == 100 && arms.frameGeneration == 2,
	    "arms carry the committing epoch and generation");
	Check(receiver.GetCameraLeg(0, leg, 1000), "left leg commits with frame");
	Check(leg.state == NetCameraLegState::WalkStep
	        && leg.sourceEpoch == 100 && leg.frameGeneration == 2,
	    "left leg carries state plus committing generation");
	Check(receiver.GetCameraLeg(1, leg, 1000), "right leg commits with frame");
	Check(leg.state == NetCameraLegState::KickExtend
	        && leg.sourceEpoch == 100 && leg.frameGeneration == 2,
	    "right kick is part of the same atomic generation");

	// The next frame intentionally omits all gait messages. Omission is an
	// explicit clear, never permission to reuse last frame's kick/arm intent.
	receiver.TestStoreTrackerPosition(0, 4.0f, 5.0f, 6.0f);
	receiver.TestCommitFrame(100, 1u << 0, flags);
	Check(receiver.GetTracker(0, tracker), "pose still commits when gait is omitted");
	Check(Near(tracker.pos[0], 4.0f), "new pose replaced prior generation");
	Check(!receiver.GetSkeletonArms(arms), "omitted arms clear at commit");
	Check(!receiver.GetCameraLeg(0, leg, 1000), "omitted left state clears at commit");
	Check(!receiver.GetCameraLeg(1, leg, 1000), "omitted right state clears at commit");
}

void TestEpochCannotAdoptOrphanKick()
{
	auto& receiver = NetworkTrackerReceiver::Instance();
	receiver.TestReset();
	const uint32_t flags = NetTrackerFrame_Continuous3D;
	receiver.TestCommitFrame(41, 0, flags);

	receiver.TestStoreCameraLeg(0, NetCameraLegState::KickExtend, 170.0f, 0.97f);
	receiver.TestCommitFrame(41, 0, flags);
	NetCameraLegSample leg;
	Check(receiver.GetCameraLeg(0, leg, 1000)
	        && leg.state == NetCameraLegState::KickExtend,
	    "same-epoch kick commits normally");

	// Simulate a semantic packet whose old frame trailer was lost, followed by
	// a new sender/session epoch. It must not be relabeled as belonging to the
	// new source.
	receiver.TestStoreCameraLeg(0, NetCameraLegState::KickExtend, 171.0f, 0.99f);
	receiver.TestStoreSkeletonArms(0.8f, 1.4f, 0.8f);
	receiver.TestCommitFrame(42, 0, flags);
	NetSkeletonArmsSample arms;
	Check(!receiver.GetCameraLeg(0, leg, 1000),
	    "epoch change discards an orphaned old kick");
	Check(!receiver.GetSkeletonArms(arms),
	    "epoch change discards orphaned arm phase");

	// After the epoch boundary, an ordinary frame in the new epoch resumes
	// semantics and tags them with the new generation.
	receiver.TestStoreCameraLeg(0, NetCameraLegState::Grounded, 176.0f, 0.93f);
	receiver.TestCommitFrame(42, 0, flags);
	NetTrackerFrameSample frame;
	receiver.GetTrackerFrame(frame, 1000);
	Check(receiver.GetCameraLeg(0, leg, 1000),
	    "new-epoch semantics resume on the following frame");
	Check(leg.state == NetCameraLegState::Grounded
	        && leg.sourceEpoch == frame.sourceEpoch
	        && leg.frameGeneration == frame.generation,
	    "resumed semantics match the active epoch/generation");
}

void Tick(WalkInPlace* w, uint64_t nowMs,
    float leftFootY, float rightFootY,
    bool leftHardware, bool rightHardware,
    bool semanticMode,
    bool leftStateValid, NetCameraLegState leftState,
    bool rightStateValid, NetCameraLegState rightState,
    bool activationHeld)
{
	WalkInPlace::TestSetNowMs(nowMs);
	w->Update(
	    true, leftFootY, true, rightFootY,
	    true, 0.50f, true, 0.50f,
	    leftHardware, rightHardware,
	    semanticMode,
	    leftStateValid, leftState,
	    rightStateValid, rightState,
	    false, 0.0f, 0.0f, -1.0f,
	    false, 0.0f, 0.0f, 0.0f,
	    false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	    false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	    activationHeld);
}

void TickCoordinatedCameraGait(WalkInPlace* w, uint64_t nowMs,
    float leftFootY, float rightFootY,
    float leftKneeY, float rightKneeY,
    float armPhase,
    NetCameraLegState leftState = NetCameraLegState::Grounded,
    NetCameraLegState rightState = NetCameraLegState::Grounded)
{
	WalkInPlace::TestSetNowMs(nowMs);
	w->Update(
	    true, leftFootY, true, rightFootY,
	    true, leftKneeY, true, rightKneeY,
	    false, false,
	    true,
	    true, leftState,
	    true, rightState,
	    false, 0.0f, 0.0f, -1.0f,
	    true, armPhase, 1.0f, 1.0f,
	    false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	    false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
	    false);
}

void TestGroundedSemanticsDoNotVetoProvenGait()
{
	WalkInPlace* w = WalkInPlace::TestCreate();
	constexpr float pi = 3.14159265358979323846f;
	uint64_t now = 10000;
	for (int i = 0; i < 650; ++i, now += 11) {
		float phase = 2.0f * pi * static_cast<float>(now - 10000) / 1000.0f;
		float wave = std::sin(phase);
		float leftLift = 0.075f * std::max(0.0f, wave);
		float rightLift = 0.075f * std::max(0.0f, -wave);
		TickCoordinatedCameraGait(w, now,
		    leftLift, rightLift,
		    0.50f + 0.045f * std::max(0.0f, wave),
		    0.50f + 0.045f * std::max(0.0f, -wave),
		    -0.10f * wave);
	}
	Check(w->AxisY() > 0.05f,
	    "proven World3D feet+arm cadence walks even while semantics remain Grounded");

	TickCoordinatedCameraGait(w, now + 11,
	    0.08f, 0.0f, 0.56f, 0.50f, 0.0f,
	    NetCameraLegState::Chamber, NetCameraLegState::Grounded);
	Check(Near(w->AxisY(), 0.0f) && !w->IsRunning(),
	    "semantic Chamber still hard-vetoes proven gait immediately");
	WalkInPlace::TestDestroy(w);
}

void TestQuickStartAndSemanticVeto()
{
	WalkInPlace* w = WalkInPlace::TestCreate();
	Tick(w, 1000, 0.0f, 0.0f,
	    false, false, false,
	    false, NetCameraLegState::Invalid,
	    false, NetCameraLegState::Invalid, true);
	Tick(w, 1011, 0.035f, 0.0f,
	    false, false, false,
	    false, NetCameraLegState::Invalid,
	    false, NetCameraLegState::Invalid, true);
	Check(w->TestQuickStartArmed(),
	    "held activation plus one clear lift arms quick start");
	Check(w->AxisY() > 0.0f,
	    "quick start produces a gentle forward preview");
	WalkInPlace::TestDestroy(w);

	w = WalkInPlace::TestCreate();
	Tick(w, 2000, 0.0f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::Grounded,
	    true, NetCameraLegState::Grounded, true);
	Tick(w, 2011, 0.035f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::UndecidedLift,
	    true, NetCameraLegState::Grounded, true);
	Check(w->TestQuickStartArmed() && w->AxisY() > 0.0f,
	    "fresh UndecidedLift permits the assigned button's first-step preview");
	Tick(w, 2017, 0.050f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::Chamber,
	    true, NetCameraLegState::Grounded, true);
	Check(!w->TestQuickStartArmed() && Near(w->AxisY(), 0.0f)
	        && !w->IsRunning(),
	    "a provisional chamber hard-pauses output without clearing history");
	Tick(w, 2022, 0.07f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::KickExtend,
	    true, NetCameraLegState::Grounded, true);
	Check(w->TestCameraKickLatched(0), "confirmed kick latches action side");
	Check(!w->TestQuickStartArmed() && Near(w->AxisY(), 0.0f),
	    "confirmed kick cancels quick start and hard-zeros movement");
	WalkInPlace::TestDestroy(w);
}

void TestInvalidTimeoutAndPerSideHardwareBypass()
{
	WalkInPlace* w = WalkInPlace::TestCreate();
	Tick(w, 5000, 0.10f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::KickExtend,
	    true, NetCameraLegState::Grounded, true);
	Check(w->TestCameraKickLatched(0), "kick begins latched quarantine");
	Tick(w, 6000, 0.0f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::Invalid,
	    true, NetCameraLegState::Invalid, true);
	Check(w->TestCameraKickLatched(0),
	    "brief Invalid dropout does not release an active kick early");
	Tick(w, 6201, 0.0f, 0.0f,
	    false, false, true,
	    true, NetCameraLegState::Invalid,
	    true, NetCameraLegState::Invalid, true);
	Check(!w->TestCameraKickLatched(0),
	    "fresh Invalid packets cannot keep a kick latched forever");
	WalkInPlace::TestDestroy(w);

	w = WalkInPlace::TestCreate();
	Tick(w, 7000, 0.10f, 0.0f,
	    true, false, true,
	    true, NetCameraLegState::KickExtend,
	    true, NetCameraLegState::Grounded, false);
	Check(!w->TestCameraKickLatched(0),
	    "physical left foot bypasses camera semantics for that side");
	Tick(w, 7011, 0.0f, 0.10f,
	    true, false, true,
	    true, NetCameraLegState::KickExtend,
	    true, NetCameraLegState::KickExtend, false);
	Check(!w->TestCameraKickLatched(0) && w->TestCameraKickLatched(1),
	    "mixed tracking bypass is per-side, not all-or-nothing");
	WalkInPlace::TestDestroy(w);
}

} // namespace

int main()
{
	TestRawMediaPipeNativePath();
	TestAtomicGaitCommitAndOmission();
	TestEpochCannotAdoptOrphanKick();
	TestGroundedSemanticsDoNotVetoProvenGait();
	TestQuickStartAndSemanticVeto();
	TestInvalidTimeoutAndPerSideHardwareBypass();

	if (failures != 0) {
		std::cerr << failures << " runtime semantics assertion(s) failed\n";
		return 1;
	}

	std::cout << "Runtime semantics self-test passed: native raw body mapping, "
	             "HMD/body yaw separation, atomic gait commits, "
	             "omission clearing, epoch isolation, Grounded semantic gait, "
	             "bounded Invalid, per-side hardware bypass, and action veto.\n";
	return 0;
}
