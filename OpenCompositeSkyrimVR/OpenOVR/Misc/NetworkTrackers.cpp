#ifdef OCU_RUNTIME_SELF_TEST
#include "NetworkTrackers.h"

// Keep the self-test executable independent of OCCore and its generated OpenVR
// headers. The production translation unit continues to use the normal logger.
#define OOVR_LOG(...) ((void)0)
#define OOVR_LOGF(...) ((void)0)
#define OOVR_LOG_ONCE(...) ((void)0)
#else
#include "stdafx.h"

#include "NetworkTrackers.h"
#endif

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

NetworkTrackerReceiver& NetworkTrackerReceiver::Instance()
{
	static NetworkTrackerReceiver instance;
	return instance;
}

NetworkTrackerReceiver::~NetworkTrackerReceiver()
{
	Stop();
}

uint64_t NetworkTrackerReceiver::NowMs()
{
	using namespace std::chrono;
	return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool NetworkTrackerReceiver::Start(int port)
{
#ifdef WIN32
	if (running.load(std::memory_order_acquire))
		return true;

	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		OOVR_LOG("Network trackers: WSAStartup failed");
		return false;
	}

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		OOVR_LOGF("Network trackers: socket() failed (%d)", WSAGetLastError());
		return false;
	}

	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((u_short)port);
	if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		OOVR_LOGF("Network trackers: bind() on UDP %d failed (%d) — port in use?", port, WSAGetLastError());
		closesocket(s);
		return false;
	}

	// Timed recv so the thread notices the stop flag without needing a
	// socket close to unblock it (avoids teardown races).
	DWORD timeoutMs = 500;
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

	sock.store((uintptr_t)s, std::memory_order_release);
	boundPort = port;
	running.store(true, std::memory_order_release);
	thread = std::thread(&NetworkTrackerReceiver::ThreadLoop, this);
	return true;
#else
	return false;
#endif
}

void NetworkTrackerReceiver::Stop()
{
#ifdef WIN32
	if (!running.exchange(false, std::memory_order_acq_rel))
		return;
	uintptr_t stoppingSocket = sock.exchange(
	    (uintptr_t)INVALID_SOCKET, std::memory_order_acq_rel);
	if ((SOCKET)stoppingSocket != INVALID_SOCKET) {
		closesocket((SOCKET)stoppingSocket);
	}
	if (thread.joinable())
		thread.join();
#endif
}

void NetworkTrackerReceiver::ThreadLoop()
{
#ifdef WIN32
	char buf[2048];
	while (running.load(std::memory_order_acquire)) {
		sockaddr_in from = {};
		int fromLen = sizeof(from);
		SOCKET receiveSocket = (SOCKET)sock.load(std::memory_order_acquire);
		if (receiveSocket == INVALID_SOCKET)
			break;
		int len = recvfrom(receiveSocket, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
		if (len <= 0)
			continue; // timeout, socket closed, or malformed — just re-check running

		if (!loggedFirstPacket) {
			loggedFirstPacket = true;
			char ip[64] = "?";
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			OOVR_LOGF("Network trackers: first OSC packet received from %s (UDP %d)", ip, boundPort);
		}

		ParsePacket(buf, len, 0);
	}
#endif
}

// ---- OSC parsing -----------------------------------------------------------

static uint32_t ReadBigU32(const char* p)
{
	return ((uint32_t)(uint8_t)p[0] << 24) | ((uint32_t)(uint8_t)p[1] << 16)
	    | ((uint32_t)(uint8_t)p[2] << 8) | (uint32_t)(uint8_t)p[3];
}

static float ReadBigF32(const char* p)
{
	uint32_t u = ReadBigU32(p);
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

static uint32_t ReadLittleU32(const char* p)
{
	return (uint32_t)(uint8_t)p[0] | ((uint32_t)(uint8_t)p[1] << 8)
	    | ((uint32_t)(uint8_t)p[2] << 16) | ((uint32_t)(uint8_t)p[3] << 24);
}

static uint64_t ReadLittleU64(const char* p)
{
	return (uint64_t)ReadLittleU32(p)
	    | ((uint64_t)ReadLittleU32(p + 4) << 32);
}

static float ReadLittleF32(const char* p)
{
	uint32_t u = ReadLittleU32(p);
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

struct RawBodyVec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct RawBodyLandmark {
	RawBodyVec3 point;
	float confidence = -1.0f;
};

static RawBodyVec3 RawAdd(const RawBodyVec3& a, const RawBodyVec3& b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static RawBodyVec3 RawSub(const RawBodyVec3& a, const RawBodyVec3& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static RawBodyVec3 RawScale(const RawBodyVec3& value, float scale)
{
	return { value.x * scale, value.y * scale, value.z * scale };
}

static bool RawFinite(const RawBodyVec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y)
	    && std::isfinite(value.z);
}

static float RawDistance(const RawBodyVec3& a, const RawBodyVec3& b)
{
	RawBodyVec3 d = RawSub(a, b);
	return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

static float RawDot(const RawBodyVec3& a, const RawBodyVec3& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float RawLength(const RawBodyVec3& value)
{
	return std::sqrt(RawDot(value, value));
}

static bool FloatToExactUint(float value, uint32_t maximum, uint32_t& out)
{
	// OSC carries this compact control header as floats to preserve the existing
	// ,fff packet encoder. Integers through 2^24 are exact in IEEE-754 float.
	if (!std::isfinite(value) || value < 0.0f || value > (float)maximum
	    || std::floor(value) != value)
		return false;
	out = static_cast<uint32_t>(value);
	return true;
}

// Returns the padded length of an OSC string starting at data (multiple of 4),
// or -1 if unterminated within len.
static int OscStringLen(const char* data, int len)
{
	for (int i = 0; i < len; i++) {
		if (data[i] == '\0')
			return (i / 4 + 1) * 4;
	}
	return -1;
}

void NetworkTrackerReceiver::ParsePacket(const char* data, int len, int depth)
{
	if (depth > 4 || len < 8)
		return;

	// OCU3 is one atomic raw MediaPipe World3D frame. It is intentionally not
	// OSC: 33 landmarks fit in one sub-MTU datagram and reach the native runtime
	// without C# tracker-coordinate conversion or per-message staging.
	if (len == 72 + 33 * 16 && memcmp(data, "OCU3", 4) == 0) {
		ParseRawMediaPipePacket(data, len);
		return;
	}

	// Bundle: "#bundle\0" + 8-byte timetag + (int32 size + element)*
	if (memcmp(data, "#bundle", 8) == 0) {
		int off = 16;
		while (off + 4 <= len) {
			uint32_t rawElemLen = ReadBigU32(data + off);
			off += 4;
			if (rawElemLen == 0 || rawElemLen > static_cast<uint32_t>(len - off))
				break;
			int elemLen = static_cast<int>(rawElemLen);
			ParsePacket(data + off, elemLen, depth + 1);
			off += elemLen;
		}
		return;
	}

	ParseMessage(data, len);
}

void NetworkTrackerReceiver::ParseRawMediaPipePacket(const char* data, int len)
{
	constexpr int HEADER_BYTES = 72;
	constexpr int LANDMARK_BYTES = 16;
	constexpr int LANDMARK_COUNT = 33;
	constexpr float MIN_CONFIDENCE = 0.25f;
	if (len != HEADER_BYTES + LANDMARK_COUNT * LANDMARK_BYTES
	    || ReadLittleU32(data + 4) != 1)
		return;

	const uint32_t sourceEpoch = ReadLittleU32(data + 8);
	const uint32_t packetFlags = ReadLittleU32(data + 12);
	const uint64_t sequence = ReadLittleU64(data + 16);
	const uint64_t sourceTimestampMs = ReadLittleU64(data + 24);
	const uint32_t rawLeftState = ReadLittleU32(data + 32);
	const uint32_t rawRightState = ReadLittleU32(data + 36);
	const float leftKneeAngle = ReadLittleF32(data + 40);
	const float leftConfidence = ReadLittleF32(data + 44);
	const float rightKneeAngle = ReadLittleF32(data + 48);
	const float rightConfidence = ReadLittleF32(data + 52);
	const float armPhase = ReadLittleF32(data + 56);
	const float leftHandY = ReadLittleF32(data + 60);
	const float rightHandY = ReadLittleF32(data + 64);
	const bool mirroredInput = (packetFlags & 1u) != 0;
	const bool armsValid = (packetFlags & (1u << 1)) != 0;

	if (sourceEpoch > 0x00ffffffu || sequence == 0 || sourceTimestampMs == 0)
		return;

	RawBodyLandmark landmarks[LANDMARK_COUNT];
	for (int i = 0; i < LANDMARK_COUNT; ++i) {
		const char* item = data + HEADER_BYTES + i * LANDMARK_BYTES;
		const float rawX = ReadLittleF32(item);
		const float rawY = ReadLittleF32(item + 4);
		const float rawZ = ReadLittleF32(item + 8);
		const float confidence = ReadLittleF32(item + 12);
		// This is the sole MediaPipe -> OCU sender-space conversion. With an
		// unmirrored front-facing camera, anatomical LEFT appears at positive
		// image/world X and anatomical RIGHT at negative X. VR playspace uses
		// +X for the user's right, so camera X must be inverted here. The later
		// Unity/OpenXR Z sign change is only the API handedness boundary.
		landmarks[i].point = {
			(mirroredInput ? 1.0f : -1.0f) * rawX,
			-rawY,
			-rawZ,
		};
		landmarks[i].confidence = confidence;
		if (!RawFinite(landmarks[i].point) || !std::isfinite(confidence))
			landmarks[i].confidence = -1.0f;
	}

	auto usable = [&](int index) {
		return landmarks[index].confidence >= MIN_CONFIDENCE;
	};
	auto center = [&](int a, int b) {
		return RawScale(RawAdd(landmarks[a].point, landmarks[b].point), 0.5f);
	};
	auto resolveFoot = [&](int ankle, int heel, int toe,
	                       RawBodyVec3& foot, float& groundY) {
		groundY = INFINITY;
		if (usable(ankle)) groundY = std::min(groundY, landmarks[ankle].point.y);
		if (usable(heel)) groundY = std::min(groundY, landmarks[heel].point.y);
		if (usable(toe)) groundY = std::min(groundY, landmarks[toe].point.y);
		if (usable(ankle)) {
			foot = landmarks[ankle].point;
			return true;
		}
		if (usable(heel) && usable(toe)) {
			foot = RawScale(RawAdd(landmarks[heel].point, landmarks[toe].point), 0.5f);
			return true;
		}
		return false;
	};

	// Stable MediaPipe indices; sides always come from labels, never X sign.
	constexpr int L_SHOULDER = 11, R_SHOULDER = 12;
	constexpr int L_ELBOW = 13, R_ELBOW = 14;
	constexpr int L_HIP = 23, R_HIP = 24;
	constexpr int L_KNEE = 25, R_KNEE = 26;
	constexpr int L_ANKLE = 27, R_ANKLE = 28;
	constexpr int L_HEEL = 29, R_HEEL = 30;
	constexpr int L_TOE = 31, R_TOE = 32;

	const bool torsoValid = usable(L_HIP) && usable(R_HIP)
	    && usable(L_SHOULDER) && usable(R_SHOULDER);
	RawBodyVec3 hipCenter{};
	RawBodyVec3 shoulderCenter{};
	if (torsoValid) {
		hipCenter = center(L_HIP, R_HIP);
		shoulderCenter = center(L_SHOULDER, R_SHOULDER);
	}
	RawBodyVec3 leftFoot{}, rightFoot{};
	float leftGroundY = INFINITY, rightGroundY = INFINITY;
	const bool leftFootValid = resolveFoot(
	    L_ANKLE, L_HEEL, L_TOE, leftFoot, leftGroundY);
	const bool rightFootValid = resolveFoot(
	    R_ANKLE, R_HEEL, R_TOE, rightFoot, rightGroundY);

	float measuredYawDeg = 0.0f;
	bool yawMeasured = false;
	RawBodyVec3 bodyForward{ 0.0f, 0.0f, 1.0f };
	bool bodyForwardValid = false;
	if (torsoValid) {
		// Average shoulder and hip right axes, then derive body forward. HMD
		// yaw is deliberately absent: the skeleton owns body orientation.
		RawBodyVec3 rightAxis = RawAdd(
		    RawSub(landmarks[R_HIP].point, landmarks[L_HIP].point),
		    RawSub(landmarks[R_SHOULDER].point, landmarks[L_SHOULDER].point));
		rightAxis.y = 0.0f;
		const float length = std::sqrt(
		    rightAxis.x * rightAxis.x + rightAxis.z * rightAxis.z);
		if (length > 0.04f && std::isfinite(length)) {
			rightAxis.x /= length;
			rightAxis.z /= length;
			bodyForward = { -rightAxis.z, 0.0f, rightAxis.x };
			bodyForwardValid = true;
			measuredYawDeg = std::atan2(bodyForward.x, bodyForward.z)
			    * (180.0f / 3.14159265358979323846f);
			yawMeasured = std::isfinite(measuredYawDeg);
		}
	}

	std::lock_guard<std::mutex> lock(mutex);
	const uint64_t now = NowMs();
	const bool firstFrame = !trackerFrame.everSeen;
	const bool epochChanged = !firstFrame && trackerFrame.sourceEpoch != sourceEpoch;
	const bool rawSourceChanged = !rawEpochValid || rawEpoch != sourceEpoch;
	const bool rawDiscontinuous = rawSourceChanged
	    || sequence <= rawSequence || sourceTimestampMs <= rawTimestampMs
	    || (rawTimestampMs != 0 && sourceTimestampMs - rawTimestampMs > 1000);
	if (rawDiscontinuous) {
		rawFloorValid = false;
		memset(rawFilterValid, 0, sizeof(rawFilterValid));
		memset(rawFiltered, 0, sizeof(rawFiltered));
		rawYawValid = false;
		rawYawDeg = 0.0f;
		if (rawSourceChanged) {
			memset(rawStandingLegReachValid, 0, sizeof(rawStandingLegReachValid));
			memset(rawStandingLegReach, 0, sizeof(rawStandingLegReach));
			memset(rawAnkleOffsetValid, 0, sizeof(rawAnkleOffsetValid));
			memset(rawAnkleOffset, 0, sizeof(rawAnkleOffset));
		}
	}
	rawEpochValid = true;
	rawEpoch = sourceEpoch;
	rawSequence = sequence;
	rawTimestampMs = sourceTimestampMs;

	auto leftState = rawLeftState <= (uint32_t)NetCameraLegState::Recover
	    ? (NetCameraLegState)rawLeftState : NetCameraLegState::Invalid;
	auto rightState = rawRightState <= (uint32_t)NetCameraLegState::Recover
	    ? (NetCameraLegState)rawRightState : NetCameraLegState::Invalid;

	// Only grounded feet update the floor once acquired. A raised knee or kick
	// must not drag the virtual floor upward and shorten the other leg.
	float floorCandidate = INFINITY;
	if (leftFootValid && (leftState == NetCameraLegState::Grounded || !rawFloorValid))
		floorCandidate = std::min(floorCandidate, leftGroundY);
	if (rightFootValid && (rightState == NetCameraLegState::Grounded || !rawFloorValid))
		floorCandidate = std::min(floorCandidate, rightGroundY);
	if (std::isfinite(floorCandidate)) {
		if (!rawFloorValid) {
			rawFloorY = floorCandidate;
			rawFloorValid = true;
		} else {
			const float step = std::abs(floorCandidate - rawFloorY);
			const float alpha = step > 0.12f ? 0.45f : 0.10f;
			rawFloorY += alpha * (floorCandidate - rawFloorY);
		}
	}

	// Learn actual standing reach before processing kicks. The user's held
	// neutral stance is the reliable length reference; a raised monocular leg
	// can lose 15-25 cm to foreshortening even while its joints look collinear.
	auto learnStandingReach = [&](int side, int hipIndex, int kneeIndex,
	                              const RawBodyVec3& foot, bool footValid) {
		if (!rawFloorValid || !footValid || !usable(hipIndex) || !usable(kneeIndex))
			return;
		const RawBodyVec3 hip = landmarks[hipIndex].point;
		const RawBodyVec3 knee = landmarks[kneeIndex].point;
		const float segmentedReach = RawDistance(hip, knee) + RawDistance(knee, foot);
		const float directReach = RawDistance(hip, foot);
		const float straightRatio = segmentedReach > 0.20f
		    ? directReach / segmentedReach : 0.0f;
		const float lift = foot.y - rawFloorY;
		if (lift > 0.10f || straightRatio < 0.90f
		    || segmentedReach < 0.50f || segmentedReach > 1.10f)
			return;

		if (!rawStandingLegReachValid[side]) {
			rawStandingLegReach[side] = segmentedReach;
			rawStandingLegReachValid[side] = true;
			return;
		}
		// Learn upward quickly and decay downward very slowly. This keeps one
		// partially occluded neutral frame from shortening the calibrated leg.
		const float alpha = segmentedReach > rawStandingLegReach[side]
		    ? 0.15f : 0.01f;
		rawStandingLegReach[side] += alpha
		    * (segmentedReach - rawStandingLegReach[side]);
	};
	learnStandingReach(0, L_HIP, L_KNEE, leftFoot, leftFootValid);
	learnStandingReach(1, R_HIP, R_KNEE, rightFoot, rightFootValid);

	// Learn each ankle's height above its own heel/toe ground point while the
	// foot is near the floor. Heel and toe rise with the ankle mid-kick, so
	// learning is restricted to grounded-adjacent frames.
	auto learnAnkleOffset = [&](int side, const RawBodyVec3& foot,
	                            bool footValid, float groundY) {
		if (!footValid || !std::isfinite(groundY))
			return;
		if (rawFloorValid && (groundY - rawFloorY) > 0.06f)
			return;
		const float offset = foot.y - groundY;
		if (offset < 0.0f || offset > 0.20f)
			return;
		if (!rawAnkleOffsetValid[side]) {
			rawAnkleOffset[side] = offset;
			rawAnkleOffsetValid[side] = true;
		} else {
			rawAnkleOffset[side] += 0.10f * (offset - rawAnkleOffset[side]);
		}
	};
	learnAnkleOffset(0, leftFoot, leftFootValid, leftGroundY);
	learnAnkleOffset(1, rightFoot, rightFootValid, rightGroundY);

	// SkyrimVR-FBT receives only waist and foot trackers; it solves the knee
	// itself. A monocular pose can visibly find a straight kick while shortening
	// the hip-to-ankle ray enough that FBT must keep the avatar knee bent. Detect
	// a raised, nearly straight 3D leg symmetrically on both sides and extend the
	// emitted foot to the measured thigh+shin reach. This does not invent a kick
	// from a knee raise: a chambered leg has a much lower reach ratio.
	auto enforceKickReach = [&](int side, int hipIndex, int kneeIndex,
	                            RawBodyVec3& foot, bool footValid,
	                            NetCameraLegState& state) {
		if (!rawFloorValid || !bodyForwardValid || !footValid
		    || !usable(hipIndex) || !usable(kneeIndex))
			return;

		const RawBodyVec3 hip = landmarks[hipIndex].point;
		const RawBodyVec3 knee = landmarks[kneeIndex].point;
		RawBodyVec3 displacement = RawSub(foot, hip);
		const float reach = RawLength(displacement);
		const float maximumReach = RawDistance(hip, knee) + RawDistance(knee, foot);
		const float reachRatio = maximumReach > 0.20f ? reach / maximumReach : 0.0f;
		// True lift: ankle height above the floor minus the ankle's natural
		// resting height above the sole. Without the offset a planted foot
		// reads ~0.10 m of permanent lift and every lift gate is decorative.
		const float ankleOffset = rawAnkleOffsetValid[side]
		    ? rawAnkleOffset[side] : 0.09f;
		const float lift = foot.y - rawFloorY - ankleOffset;
		const float horizontalReach = std::sqrt(
		    displacement.x * displacement.x + displacement.z * displacement.z);
		const bool geometricKick = lift >= 0.075f && horizontalReach >= 0.16f
		    && reachRatio >= 0.90f && maximumReach >= 0.45f;
		// The sender may confirm a kick the monocular geometry foreshortens,
		// but a leg that measures far from straight with its foot near the
		// floor is classifier jitter, not a strike.
		const bool senderConfirmedKick = state == NetCameraLegState::KickExtend
		    && lift >= 0.075f && horizontalReach >= 0.16f
		    && reachRatio >= 0.80f;

		if (!geometricKick && !senderConfirmedKick) {
			// A leg whose ankle sits at its resting height cannot be
			// chambering, kicking or recovering. Low-frame-rate camera jitter
			// makes the sender latch these states while standing, which both
			// blocks gait arbitration and arms the reach solver. Ground the
			// contradiction at the wire boundary; WalkStep is left alone
			// because a casual stride's real lift is only a few centimeters.
			if (lift < 0.035f
			    && (state == NetCameraLegState::UndecidedLift
			        || state == NetCameraLegState::Chamber
			        || state == NetCameraLegState::KneeHold
			        || state == NetCameraLegState::KickExtend
			        || state == NetCameraLegState::Recover))
				state = NetCameraLegState::Grounded;
			else if (state == NetCameraLegState::KickExtend)
				// A kick label that fails both the geometric and the
				// sender-confirmed gate is not actionable anywhere: the reach
				// solver ignores it, so the locomotion arbiter must not wipe
				// walking cadence for it either. Low-frame-rate walking reads
				// as KickExtend/Recover almost continuously, and each edge
				// wiped the step evidence before gait could ever confirm.
				state = NetCameraLegState::UndecidedLift;
			return;
		}

		state = NetCameraLegState::KickExtend;
		float forwardProjection = RawDot(displacement, bodyForward);
		if (forwardProjection < -0.04f) {
			// The user is facing the camera. Monocular depth occasionally chooses
			// the equally plausible backward solution; reflect only that component
			// while retaining lift and lateral kick direction.
			displacement = RawSub(displacement,
			    RawScale(bodyForward, 2.0f * forwardProjection));
		}

		const float correctedLength = RawLength(displacement);
		// Never derive kick length from the kick frame itself when a grounded
		// standing reference exists. A small overreach is deliberate: VRIK clamps
		// it to the avatar's real leg length, yielding a straight knee instead of
		// preserving the camera frame's foreshortened bend.
		const float referenceReach = rawStandingLegReachValid[side]
		    ? rawStandingLegReach[side] * 1.04f
		    : maximumReach * 0.985f;
		const float desiredReach = std::max(reach, referenceReach);
		if (correctedLength > 0.10f && std::isfinite(desiredReach))
			foot = RawAdd(hip, RawScale(displacement, desiredReach / correctedLength));
	};
	enforceKickReach(0, L_HIP, L_KNEE, leftFoot, leftFootValid, leftState);
	enforceKickReach(1, R_HIP, R_KNEE, rightFoot, rightFootValid, rightState);

	if (yawMeasured) {
		if (!rawYawValid) {
			rawYawDeg = measuredYawDeg;
			rawYawValid = true;
		} else {
			float delta = measuredYawDeg - rawYawDeg;
			while (delta > 180.0f) delta -= 360.0f;
			while (delta < -180.0f) delta += 360.0f;
			const float alpha = std::abs(delta) > 35.0f ? 0.45f : 0.18f;
			rawYawDeg += alpha * delta;
			while (rawYawDeg > 180.0f) rawYawDeg -= 360.0f;
			while (rawYawDeg < -180.0f) rawYawDeg += 360.0f;
		}
	}

	constexpr uint32_t frameFlags = NetTrackerFrame_UseHeadTranslation
	    | NetTrackerFrame_UseHeadHeightScale | NetTrackerFrame_UseHeadYaw
	    | NetTrackerFrame_AllowGaitFootRelease | NetTrackerFrame_Continuous3D
	    | NetTrackerFrame_HeadXZIsBodyRoot;
	const bool alignmentChanged = firstFrame
	    || ((trackerFrame.flags ^ frameFlags)
	        & (NetTrackerFrame_UseHeadTranslation
	            | NetTrackerFrame_UseHeadHeightScale | NetTrackerFrame_UseHeadYaw
	            | NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_HeadXZIsBodyRoot)) != 0;
	if (epochChanged || alignmentChanged) {
		alignValid = false;
		memset(alignOffset, 0, sizeof(alignOffset));
		alignScale = 1.0f;
		alignScaleValid = false;
		alignYawValid = false;
		alignYawRad = 0.0f;
	}

	const bool coreValid = torsoValid && leftFootValid && rightFootValid
	    && rawFloorValid && rawYawValid;
	RawBodyVec3 outputs[MAX_TRACKERS]{};
	bool outputValid[MAX_TRACKERS]{};
	RawBodyVec3 filteredHead{};
	bool headValid = false;
	if (coreValid) {
		auto floorRelative = [&](RawBodyVec3 point) {
			point.y -= rawFloorY;
			return point;
		};
		auto filter = [&](int filterIndex, const RawBodyVec3& sample,
		                  float restAlpha, float fastAlpha, float fastStep) {
			RawBodyVec3 value{
				rawFiltered[filterIndex][0],
				rawFiltered[filterIndex][1],
				rawFiltered[filterIndex][2],
			};
			if (!rawFilterValid[filterIndex]) {
				value = sample;
				rawFilterValid[filterIndex] = true;
			} else {
				const float distance = RawDistance(sample, value);
				const float alpha = distance >= fastStep ? fastAlpha : restAlpha;
				value.x += alpha * (sample.x - value.x);
				value.y += alpha * (sample.y - value.y);
				value.z += alpha * (sample.z - value.z);
			}
			rawFiltered[filterIndex][0] = value.x;
			rawFiltered[filterIndex][1] = value.y;
			rawFiltered[filterIndex][2] = value.z;
			return value;
		};

		const RawBodyVec3 rawHead = {
			hipCenter.x,
			shoulderCenter.y + 0.24f - rawFloorY,
			hipCenter.z,
		};
		filteredHead = filter(8, rawHead, 0.14f, 0.70f, 0.08f);
		headValid = true;

		RawBodyVec3 rawPoints[MAX_TRACKERS] = {
			floorRelative(hipCenter), floorRelative(leftFoot), floorRelative(rightFoot),
			floorRelative(landmarks[L_KNEE].point), floorRelative(landmarks[R_KNEE].point),
			floorRelative(landmarks[L_ELBOW].point), floorRelative(landmarks[R_ELBOW].point),
			floorRelative(shoulderCenter),
		};
		bool rawValid[MAX_TRACKERS] = {
			true, true, true, usable(L_KNEE), usable(R_KNEE),
			usable(L_ELBOW), usable(R_ELBOW), true,
		};
		for (int slot = 0; slot < MAX_TRACKERS; ++slot) {
			if (!rawValid[slot]) {
				rawFilterValid[slot] = false;
				continue;
			}
			const RawBodyVec3 relative = RawSub(rawPoints[slot], rawHead);
			const bool foot = slot == 1 || slot == 2;
			const RawBodyVec3 filteredRelative = filter(slot, relative,
			    foot ? 0.23f : 0.18f, foot ? 0.92f : 0.75f,
			    foot ? 0.055f : 0.070f);
			outputs[slot] = RawAdd(filteredHead, filteredRelative);
			outputValid[slot] = true;
		}
	} else {
		memset(rawFilterValid, 0, sizeof(rawFilterValid));
	}

	const uint64_t generation = firstFrame ? 1 : trackerFrame.generation + 1;
	uint32_t poseMask = 0;
	auto publish = [&](NetTrackerSample& destination, const RawBodyVec3& point,
	                   bool positionValid, bool rotationValid) {
		NetTrackerSample previous = destination;
		NetTrackerSample next{};
		if (positionValid && RawFinite(point)) {
			next.pos[0] = point.x;
			next.pos[1] = point.y;
			next.pos[2] = point.z;
			next.positionSeen = true;
			next.everSeen = true;
			if (previous.positionSeen && now > previous.lastUpdateMs) {
				const float dt = (now - previous.lastUpdateMs) / 1000.0f;
				if (dt > 0.0f && dt < 0.5f) {
					for (int axis = 0; axis < 3; ++axis)
						next.vel[axis] = (next.pos[axis] - previous.pos[axis]) / dt;
				}
			}
		}
		if (rotationValid) {
			next.eulerDeg[0] = 0.0f;
			next.eulerDeg[1] = rawYawDeg;
			next.eulerDeg[2] = 0.0f;
			next.rotationSeen = true;
			next.everSeen = true;
		}
		if (next.everSeen) next.lastUpdateMs = now;
		destination = next;
	};

	for (int slot = 0; slot < MAX_TRACKERS; ++slot) {
		const bool rotate = coreValid && slot <= 2 && outputValid[slot];
		publish(trackers[slot], outputs[slot], coreValid && outputValid[slot], rotate);
		if (trackers[slot].positionSeen) poseMask |= 1u << slot;
		if (trackers[slot].rotationSeen) poseMask |= 1u << (slot + 8);
		pendingTrackers[slot] = {};
	}
	publish(head, filteredHead, coreValid && headValid, coreValid && headValid);
	if (head.positionSeen) poseMask |= NET_TRACKER_HEAD_POSITION_BIT;
	if (head.rotationSeen) poseMask |= NET_TRACKER_HEAD_ROTATION_BIT;
	pendingHead = {};

	auto publishLeg = [&](int side, NetCameraLegState state,
	                      float kneeAngle, float confidence) {
		NetCameraLegSample sample{};
		sample.everSeen = true;
		sample.state = state;
		sample.kneeAngleDeg = std::isfinite(kneeAngle) ? kneeAngle : -1.0f;
		sample.confidence = std::isfinite(confidence)
		    ? std::clamp(confidence, 0.0f, 1.0f) : 0.0f;
		sample.sourceEpoch = sourceEpoch;
		sample.frameGeneration = generation;
		sample.lastUpdateMs = now;
		cameraLegs[side] = sample;
		pendingCameraLegs[side] = {};
	};
	publishLeg(0, leftState, leftKneeAngle, leftConfidence);
	publishLeg(1, rightState, rightKneeAngle, rightConfidence);
	if (armsValid && std::isfinite(armPhase)
	    && std::isfinite(leftHandY) && std::isfinite(rightHandY)) {
		skeletonArms.everSeen = true;
		skeletonArms.armPhase = armPhase;
		skeletonArms.handY[0] = leftHandY;
		skeletonArms.handY[1] = rightHandY;
		skeletonArms.sourceEpoch = sourceEpoch;
		skeletonArms.frameGeneration = generation;
		skeletonArms.lastUpdateMs = now;
	} else {
		skeletonArms = {};
	}
	pendingSkeletonArms = {};

	trackerFrame.everSeen = true;
	trackerFrame.sourceEpoch = sourceEpoch;
	trackerFrame.poseMask = poseMask;
	trackerFrame.flags = frameFlags;
	trackerFrame.generation = generation;
	trackerFrame.lastUpdateMs = now;
	OOVR_LOG_ONCE("Network trackers: direct native OCU3 MediaPipe body path active");
}

void NetworkTrackerReceiver::ParseMessage(const char* data, int len)
{
	int addrLen = OscStringLen(data, len);
	if (addrLen < 0)
		return;
	const char* addr = data;

	// Every OCU tracker/gait message currently carries three floats. Parse the
	// common OSC payload once before routing by address.
	const char* tags = data + addrLen;
	int tagsLen = OscStringLen(tags, len - addrLen);
	if (tagsLen < 0 || strncmp(tags, ",fff", 4) != 0)
		return;
	const char* args = tags + tagsLen;
	if (args + 12 > data + len)
		return;
	float xyz[3] = { ReadBigF32(args), ReadBigF32(args + 4), ReadBigF32(args + 8) };

	if (strcmp(addr, "/tracking/trackers/frame") == 0) {
		StoreTrackerFrame(xyz);
		return;
	}

	if (strcmp(addr, "/tracking/gait/arms") == 0) {
		StoreSkeletonArms(xyz);
		return;
	}

	if (strcmp(addr, "/tracking/gait/leg/left") == 0) {
		StoreCameraLeg(0, xyz);
		return;
	}

	if (strcmp(addr, "/tracking/gait/leg/right") == 0) {
		StoreCameraLeg(1, xyz);
		return;
	}

	// /tracking/trackers/<slot>/<position|rotation>, slot = 1..8 or "head"
	static const char PREFIX[] = "/tracking/trackers/";
	if (strncmp(addr, PREFIX, sizeof(PREFIX) - 1) != 0)
		return;
	const char* rest = addr + sizeof(PREFIX) - 1;

	int slot;
	if (strncmp(rest, "head/", 5) == 0) {
		slot = -1;
		rest += 5;
	} else if (strncmp(rest, "lhand/", 6) == 0) {
		slot = -2; // alignment reference only
		rest += 6;
	} else if (strncmp(rest, "rhand/", 6) == 0) {
		slot = -3; // alignment reference only
		rest += 6;
	} else if (rest[0] >= '1' && rest[0] <= '8' && rest[1] == '/') {
		slot = rest[0] - '1';
		rest += 2;
	} else {
		return;
	}

	bool isRotation;
	if (strcmp(rest, "position") == 0)
		isRotation = false;
	else if (strcmp(rest, "rotation") == 0)
		isRotation = true;
	else
		return;

	StoreSample(slot, isRotation, xyz);
}

void NetworkTrackerReceiver::StoreSample(int slot, bool isRotation, const float xyz[3])
{
	if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
		return;
	std::lock_guard<std::mutex> lock(mutex);
	uint64_t now = NowMs();

	// A different legacy sender may take over the port after a framed sender
	// exits. Once the framed heartbeat has been absent for two seconds, the next
	// ordinary pose packet returns the receiver to direct legacy mode.
	if (trackerFrame.everSeen && now >= trackerFrame.lastUpdateMs
	    && now - trackerFrame.lastUpdateMs > 2000) {
		trackerFrame = {};
		memset(trackers, 0, sizeof(trackers));
		head = {};
		memset(pendingTrackers, 0, sizeof(pendingTrackers));
		pendingHead = {};
		alignValid = false;
		memset(alignOffset, 0, sizeof(alignOffset));
		alignScale = 1.0f;
		alignScaleValid = false;
		alignYawValid = false;
		alignYawRad = 0.0f;
	}

	auto update = [&](NetTrackerSample& t) {
		if (isRotation) {
			memcpy(t.eulerDeg, xyz, sizeof(t.eulerDeg));
			t.rotationSeen = true;
			t.everSeen = true;
			// A rotation-only IMU sender remains alive on every rotation packet.
			if (!t.positionSeen)
				t.lastUpdateMs = now;
			return;
		}

		if (t.positionSeen && now >= t.lastUpdateMs) {
			float dt = (now - t.lastUpdateMs) / 1000.0f;
			if (dt > 0.0f && dt < 0.5f) {
				for (int i = 0; i < 3; i++)
					t.vel[i] = (xyz[i] - t.pos[i]) / dt;
			} else {
				memset(t.vel, 0, sizeof(t.vel));
			}
		}
		memcpy(t.pos, xyz, sizeof(t.pos));
		t.positionSeen = true;
		t.everSeen = true;
		t.lastUpdateMs = now;
	};

	// Wrist references are diagnostics/alignment aids only and are not part of
	// the public eight-slot atomic frame.
	if (slot == -2 || slot == -3) {
		update(slot == -2 ? lhand : rhand);
		return;
	}

	NetTrackerSample& active = slot == -1 ? head : trackers[slot];
	NetTrackerSample& pending = slot == -1 ? pendingHead : pendingTrackers[slot];
	if (trackerFrame.everSeen) {
		update(pending);
	} else {
		// Preserve exact no-header behavior while retaining a copy so the first
		// frame heartbeat (which arrives last) can commit what preceded it.
		update(active);
		pending = active;
	}
}

void NetworkTrackerReceiver::StoreTrackerFrame(const float values[3])
{
	uint32_t sourceEpoch = 0;
	uint32_t poseMask = 0;
	uint32_t flags = 0;
	if (!FloatToExactUint(values[0], 0x00ffffffu, sourceEpoch)
	    || !FloatToExactUint(values[1], NET_TRACKER_POSE_MASK, poseMask)
	    || !FloatToExactUint(values[2], 0x00ffffffu, flags))
		return;

	// Normalize reserved/legacy policy here. FollowHmdYaw remains valid for
	// Continuous3D: alignment solves real HMD yaw minus sender body yaw, so a
	// measured turn is not doubled and a missed camera turn cannot strand the
	// lower body at its original calibration heading.
	const uint32_t normalizedFlags = NetTrackerFrameNormalizePolicyFlags(flags);
	if (normalizedFlags != flags) {
		OOVR_LOG_ONCE("Network trackers: normalized tracker-frame policy flags");
		flags = normalizedFlags;
	}

	std::lock_guard<std::mutex> lock(mutex);
	const uint64_t now = NowMs();
	const bool firstFrame = !trackerFrame.everSeen;
	const bool epochChanged = !firstFrame && trackerFrame.sourceEpoch != sourceEpoch;
	const uint64_t frameGeneration = firstFrame ? 1 : trackerFrame.generation + 1;
	constexpr uint32_t alignmentFlags = NetTrackerFrame_UseHeadTranslation
	    | NetTrackerFrame_UseHeadHeightScale | NetTrackerFrame_UseHeadYaw
	    | NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_HeadXZIsBodyRoot;
	const bool alignmentChanged = firstFrame
	    || ((trackerFrame.flags ^ flags) & alignmentFlags) != 0;

	if (epochChanged || alignmentChanged) {
		alignValid = false;
		memset(alignOffset, 0, sizeof(alignOffset));
		alignScale = 1.0f;
		alignScaleValid = false;
		alignYawValid = false;
		alignYawRad = 0.0f;
	}

	auto commit = [&](NetTrackerSample& active, NetTrackerSample& pending,
	                  bool wantPosition, bool wantRotation) {
		NetTrackerSample next{};
		if (wantPosition && pending.positionSeen) {
			memcpy(next.pos, pending.pos, sizeof(next.pos));
			next.positionSeen = true;
			next.everSeen = true;
			if (!epochChanged && !firstFrame && active.positionSeen
			    && now >= active.lastUpdateMs) {
				float dt = (now - active.lastUpdateMs) / 1000.0f;
				if (dt > 0.0f && dt < 0.5f) {
					for (int axis = 0; axis < 3; axis++)
						next.vel[axis] = (next.pos[axis] - active.pos[axis]) / dt;
				}
			}
		}
		if (wantRotation && pending.rotationSeen) {
			memcpy(next.eulerDeg, pending.eulerDeg, sizeof(next.eulerDeg));
			next.rotationSeen = true;
			next.everSeen = true;
		}
		if (next.everSeen)
			next.lastUpdateMs = now;
		active = next;
		pending = {};
	};

	for (int slot = 0; slot < MAX_TRACKERS; slot++) {
		commit(trackers[slot], pendingTrackers[slot],
		    (poseMask & (1u << slot)) != 0,
		    (poseMask & (1u << (slot + 8))) != 0);
	}
	commit(head, pendingHead,
	    (poseMask & NET_TRACKER_HEAD_POSITION_BIT) != 0,
	    (poseMask & NET_TRACKER_HEAD_ROTATION_BIT) != 0);

	// Gait semantics precede the frame header in the OSC bundle just like pose
	// fields. Publish them only at this same commit point; an omitted field
	// explicitly clears the previous frame instead of leaving stale intent.
	auto commitGait = [&](auto& active, auto& pending) {
		// Messages do not carry their own epoch until this trailing header.
		// On an epoch transition, pending data could be an orphan from the old
		// sender (for example, its header datagram was lost). Discard the first
		// semantic set conservatively; the new sender republishes next frame.
		if (epochChanged) {
			active = {};
		} else if (pending.everSeen) {
			active = pending;
			active.sourceEpoch = sourceEpoch;
			active.frameGeneration = frameGeneration;
			active.lastUpdateMs = now;
		} else {
			active = {};
		}
		pending = {};
	};
	commitGait(skeletonArms, pendingSkeletonArms);
	commitGait(cameraLegs[0], pendingCameraLegs[0]);
	commitGait(cameraLegs[1], pendingCameraLegs[1]);

	trackerFrame.everSeen = true;
	trackerFrame.sourceEpoch = sourceEpoch;
	trackerFrame.poseMask = poseMask;
	trackerFrame.flags = flags;
	trackerFrame.generation = frameGeneration;
	trackerFrame.lastUpdateMs = now;

	if (firstFrame || epochChanged) {
		OOVR_LOGF("Network trackers: framed OSC source epoch %u, mask 0x%05x, flags 0x%x",
		    sourceEpoch, poseMask, flags);
	}
}

void NetworkTrackerReceiver::StoreSkeletonArms(const float xyz[3])
{
	if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2]))
		return;
	NetSkeletonArmsSample next;
	next.armPhase = xyz[0];
	next.handY[0] = xyz[1];
	next.handY[1] = xyz[2];
	next.everSeen = true;
	next.lastUpdateMs = NowMs();
	std::lock_guard<std::mutex> lock(mutex);
	if (trackerFrame.everSeen) {
		pendingSkeletonArms = next;
	} else {
		// Exact compatibility for no-header OSC senders, while retaining the
		// same value for a first framed commit that follows this message.
		skeletonArms = next;
		pendingSkeletonArms = next;
	}
}

void NetworkTrackerReceiver::StoreCameraLeg(int side, const float values[3])
{
	if (side < 0 || side >= 2)
		return;

	int stateCode = 0;
	if (std::isfinite(values[0])) {
		stateCode = static_cast<int>(std::lround(values[0]));
		if (stateCode < static_cast<int>(NetCameraLegState::Invalid)
		    || stateCode > static_cast<int>(NetCameraLegState::Recover)) {
			stateCode = static_cast<int>(NetCameraLegState::Invalid);
		}
	}

	float confidence = std::isfinite(values[2]) ? values[2] : 0.0f;
	if (confidence < 0.0f)
		confidence = 0.0f;
	else if (confidence > 1.0f)
		confidence = 1.0f;

	NetCameraLegSample next;
	next.everSeen = true;
	next.state = static_cast<NetCameraLegState>(stateCode);
	next.kneeAngleDeg = std::isfinite(values[1]) ? values[1] : -1.0f;
	next.confidence = confidence;
	next.lastUpdateMs = NowMs();

	std::lock_guard<std::mutex> lock(mutex);
	if (trackerFrame.everSeen) {
		pendingCameraLegs[side] = next;
	} else {
		cameraLegs[side] = next;
		pendingCameraLegs[side] = next;
	}
}

// ---- Reader API ------------------------------------------------------------

bool NetworkTrackerReceiver::GetTracker(int idx, NetTrackerSample& out,
    NetTrackerFrameSample* frameOut)
{
	if (idx < 0 || idx >= MAX_TRACKERS)
		return false;
	std::lock_guard<std::mutex> lock(mutex);
	out = trackers[idx];
	if (frameOut)
		*frameOut = trackerFrame;
	return out.everSeen;
}

bool NetworkTrackerReceiver::GetHead(NetTrackerSample& out)
{
	std::lock_guard<std::mutex> lock(mutex);
	out = head;
	return out.everSeen;
}

bool NetworkTrackerReceiver::GetTrackerFrame(NetTrackerFrameSample& out, uint64_t maxAgeMs)
{
	std::lock_guard<std::mutex> lock(mutex);
	out = trackerFrame;
	if (!out.everSeen)
		return false;
	const uint64_t now = NowMs();
	return now >= out.lastUpdateMs && now - out.lastUpdateMs <= maxAgeMs;
}

bool NetworkTrackerReceiver::GetSkeletonArms(NetSkeletonArmsSample& out)
{
	std::lock_guard<std::mutex> lock(mutex);
	out = skeletonArms;
	return out.everSeen;
}

bool NetworkTrackerReceiver::GetCameraLeg(int side, NetCameraLegSample& out, uint64_t maxAgeMs)
{
	if (side < 0 || side >= 2)
		return false;

	std::lock_guard<std::mutex> lock(mutex);
	out = cameraLegs[side];
	if (!out.everSeen)
		return false;

	const uint64_t nowMs = NowMs();
	return nowMs >= out.lastUpdateMs && nowMs - out.lastUpdateMs <= maxAgeMs;
}

void NetworkTrackerReceiver::UpdateAlignment(const float hmdPosStanding[3],
    const float hmdForwardStanding[2], const float* lctrl, const float* rctrl)
{
	std::lock_guard<std::mutex> lock(mutex);
	const uint64_t now = NowMs();
	(void)lctrl;
	(void)rctrl;

	const bool framed = trackerFrame.everSeen;
	const bool frameFresh = framed && now >= trackerFrame.lastUpdateMs
	    && now - trackerFrame.lastUpdateMs <= 1500;
	// Once a sender opts into the framed contract, a missing heartbeat means
	// "untracked", not permission to reinterpret stale 3D data as legacy gait.
	if (framed && !frameFresh)
		return;

	const uint32_t frameFlags = framed ? trackerFrame.flags : 0;
	const bool useTranslation = !framed
	    || (frameFlags & NetTrackerFrame_UseHeadTranslation) != 0;
	const bool useScale = !framed
	    || (frameFlags & NetTrackerFrame_UseHeadHeightScale) != 0;
	const bool useYaw = !framed
	    || (frameFlags & NetTrackerFrame_UseHeadYaw) != 0;
	if (!useTranslation && !useScale && !useYaw)
		return;

	const bool headFresh = head.everSeen && now >= head.lastUpdateMs
	    && now - head.lastUpdateMs <= 2000;
	if (!headFresh)
		return;

	const float alphaScale = 0.006f; // calibration only; keep HMD bob out
	bool yawJustLocked = false;

	// Framed sources explicitly declare whether their body-local frame follows
	// HMD yaw. Only no-header senders retain the old gait-packet heuristic.
	bool followHmdYaw = framed
	    && (frameFlags & NetTrackerFrame_FollowHmdYaw) != 0;
	if (!framed) {
		for (const NetCameraLegSample& leg : cameraLegs) {
			if (leg.everSeen && now >= leg.lastUpdateMs
			    && now - leg.lastUpdateMs <= 500) {
				followHmdYaw = true;
				break;
			}
		}
	}
	if (useYaw && head.rotationSeen && (!alignYawValid || followHmdYaw)) {
		float fx = hmdForwardStanding[0];
		float fz = hmdForwardStanding[1];
		float len2 = fx * fx + fz * fz;
		if (len2 > 0.25f) {
			constexpr float PI = 3.14159265358979323846f;
			float desiredYaw = std::atan2(-fx, -fz);
			float senderYaw = -head.eulerDeg[1] * (PI / 180.0f);
			float targetYaw = desiredYaw - senderYaw;
			while (targetYaw > PI) targetYaw -= 2.0f * PI;
			while (targetYaw < -PI) targetYaw += 2.0f * PI;
			if (!alignYawValid) {
				alignYawRad = targetYaw;
				alignYawValid = true;
				yawJustLocked = true;
			} else if (followHmdYaw) {
				float delta = targetYaw - alignYawRad;
				while (delta > PI) delta -= 2.0f * PI;
				while (delta < -PI) delta += 2.0f * PI;
				alignYawRad += 0.08f * delta;
				while (alignYawRad > PI) alignYawRad -= 2.0f * PI;
				while (alignYawRad < -PI) alignYawRad += 2.0f * PI;
			}
		}
	}

	// Auto height calibration: both the sender's head Y and the real HMD Y
	// are floor-referenced, so their ratio corrects the sender's height
	// guess. Only trust plausible standing samples.
	if (useScale && head.positionSeen
	    && head.pos[1] > 0.5f && hmdPosStanding[1] > 0.5f) {
		float targetScale = hmdPosStanding[1] / head.pos[1];
		if (targetScale > 0.5f && targetScale < 2.0f) {
			if (!alignScaleValid) {
				alignScale = targetScale;
				alignScaleValid = true;
			} else {
				alignScale += alphaScale * (targetScale - alignScale);
			}
		}
	}

	if (!useTranslation || !head.positionSeen)
		return;

	// Root translation must follow the real HMD, not a two-second average of
	// the HMD plus two swinging wrists. The old all-reference EMA left the
	// virtual waist/feet behind during room-scale movement, stretching the
	// avatar from the pelvis down. Sender head vs real HMD is the only stable
	// root pair. Horizontal/depth movement follows quickly; vertical remains
	// slow so ordinary head bob does not lift the whole skeleton.
	const float effectiveScale = useScale && alignScaleValid ? alignScale : 1.0f;
	const float effectiveYaw = useYaw && alignYawValid ? alignYawRad : 0.0f;
	float senderHeadX = effectiveScale * head.pos[0];
	float senderHeadZ = effectiveScale * (-head.pos[2]);
	float c = std::cos(effectiveYaw);
	float s = std::sin(effectiveYaw);
	float rotatedHeadX = c * senderHeadX + s * senderHeadZ;
	float rotatedHeadZ = -s * senderHeadX + c * senderHeadZ;
	float target[3] = {
		hmdPosStanding[0] - rotatedHeadX,
		hmdPosStanding[1] - effectiveScale * head.pos[1],
		hmdPosStanding[2] - rotatedHeadZ,
	};

	if (!alignValid || yawJustLocked) {
		memcpy(alignOffset, target, sizeof(alignOffset));
		alignValid = true;
		return;
	}

	// A sender carrying HeadXZIsBodyRoot guarantees that its synthetic head X/Z
	// is exactly its hip/body root. Apply the full HMD-minus-head correction each
	// frame so common camera drift cannot leave the waist and legs behind the
	// headset. Unmarked/legacy heads keep their noise-damping EMA.
	const float alphaHorizontal =
	    NetTrackerFrameUsesHardHorizontalRootAlignment(frameFlags) ? 1.0f : 0.30f;
	const float alphaVertical = 0.006f;
	alignOffset[0] += alphaHorizontal * (target[0] - alignOffset[0]);
	alignOffset[1] += alphaVertical * (target[1] - alignOffset[1]);
	alignOffset[2] += alphaHorizontal * (target[2] - alignOffset[2]);
}

bool NetworkTrackerReceiver::GetAlignmentOffset(float out[3])
{
	std::lock_guard<std::mutex> lock(mutex);
	if (trackerFrame.everSeen) {
		const uint64_t now = NowMs();
		const bool fresh = now >= trackerFrame.lastUpdateMs
		    && now - trackerFrame.lastUpdateMs <= 1500;
		if (!fresh
		    || (trackerFrame.flags & NetTrackerFrame_UseHeadTranslation) == 0) {
			memset(out, 0, 3 * sizeof(float));
			return false;
		}
	}
	if (!alignValid) {
		out[0] = out[1] = out[2] = 0.0f;
		return false;
	}
	memcpy(out, alignOffset, 3 * sizeof(float));
	return true;
}

float NetworkTrackerReceiver::GetAlignmentScale()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (trackerFrame.everSeen) {
		const uint64_t now = NowMs();
		const bool fresh = now >= trackerFrame.lastUpdateMs
		    && now - trackerFrame.lastUpdateMs <= 1500;
		if (!fresh
		    || (trackerFrame.flags & NetTrackerFrame_UseHeadHeightScale) == 0)
			return 1.0f;
	}
	return alignScaleValid ? alignScale : 1.0f;
}

float NetworkTrackerReceiver::GetAlignmentYaw()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (trackerFrame.everSeen) {
		const uint64_t now = NowMs();
		const bool fresh = now >= trackerFrame.lastUpdateMs
		    && now - trackerFrame.lastUpdateMs <= 1500;
		if (!fresh || (trackerFrame.flags & NetTrackerFrame_UseHeadYaw) == 0)
			return 0.0f;
	}
	return alignYawValid ? alignYawRad : 0.0f;
}

bool NetworkTrackerReceiver::IsAlignmentReady(uint32_t sourceEpoch)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!trackerFrame.everSeen || trackerFrame.sourceEpoch != sourceEpoch)
		return false;
	const uint64_t now = NowMs();
	if (now < trackerFrame.lastUpdateMs || now - trackerFrame.lastUpdateMs > 1500)
		return false;
	if ((trackerFrame.flags & NetTrackerFrame_UseHeadTranslation) != 0 && !alignValid)
		return false;
	if ((trackerFrame.flags & NetTrackerFrame_UseHeadHeightScale) != 0 && !alignScaleValid)
		return false;
	if ((trackerFrame.flags & NetTrackerFrame_UseHeadYaw) != 0 && !alignYawValid)
		return false;
	return true;
}

#ifdef OCU_RUNTIME_SELF_TEST
void NetworkTrackerReceiver::TestReset()
{
	Stop();
	std::lock_guard<std::mutex> lock(mutex);
	memset(trackers, 0, sizeof(trackers));
	head = {};
	memset(pendingTrackers, 0, sizeof(pendingTrackers));
	pendingHead = {};
	trackerFrame = {};
	lhand = {};
	rhand = {};
	skeletonArms = {};
	pendingSkeletonArms = {};
	cameraLegs[0] = {};
	cameraLegs[1] = {};
	pendingCameraLegs[0] = {};
	pendingCameraLegs[1] = {};
	rawEpochValid = false;
	rawEpoch = 0;
	rawSequence = 0;
	rawTimestampMs = 0;
	rawFloorValid = false;
	rawFloorY = 0.0f;
	memset(rawStandingLegReachValid, 0, sizeof(rawStandingLegReachValid));
	memset(rawStandingLegReach, 0, sizeof(rawStandingLegReach));
	memset(rawAnkleOffsetValid, 0, sizeof(rawAnkleOffsetValid));
	memset(rawAnkleOffset, 0, sizeof(rawAnkleOffset));
	memset(rawFilterValid, 0, sizeof(rawFilterValid));
	memset(rawFiltered, 0, sizeof(rawFiltered));
	rawYawValid = false;
	rawYawDeg = 0.0f;
	alignValid = false;
	memset(alignOffset, 0, sizeof(alignOffset));
	alignScale = 1.0f;
	alignScaleValid = false;
	alignYawValid = false;
	alignYawRad = 0.0f;
	boundPort = 0;
	loggedFirstPacket = false;
}

void NetworkTrackerReceiver::TestStoreTrackerPosition(
    int idx, float x, float y, float z)
{
	float values[3] = { x, y, z };
	StoreSample(idx, false, values);
}

void NetworkTrackerReceiver::TestStoreSkeletonArms(
    float phase, float leftHandY, float rightHandY)
{
	float values[3] = { phase, leftHandY, rightHandY };
	StoreSkeletonArms(values);
}

void NetworkTrackerReceiver::TestStoreCameraLeg(int side,
    NetCameraLegState state, float kneeAngleDeg, float confidence)
{
	float values[3] = {
		static_cast<float>(state), kneeAngleDeg, confidence
	};
	StoreCameraLeg(side, values);
}

void NetworkTrackerReceiver::TestCommitFrame(
    uint32_t sourceEpoch, uint32_t poseMask, uint32_t flags)
{
	float values[3] = {
		static_cast<float>(sourceEpoch),
		static_cast<float>(poseMask),
		static_cast<float>(flags),
	};
	StoreTrackerFrame(values);
}
#endif
