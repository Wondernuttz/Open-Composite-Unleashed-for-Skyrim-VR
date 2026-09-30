#pragma once

// VRChat-style OSC tracker receiver.
//
// Listens on UDP (default port 9000) for the de-facto standard OSC tracker
// feed: /tracking/trackers/{1..8}/position + /rotation, plus
// /tracking/trackers/head/{position,rotation} for playspace alignment. New OCU
// senders finish each snapshot with /tracking/trackers/frame so validity,
// alignment policy and source changes are committed atomically.
// SlimeVR ("OSC Trackers" output), Standable and various phone IMU apps all
// emit this, which gives SteamVR-driver-only tracker ecosystems a way into
// OCU where there is no vrserver for their driver to live in.
//
// Samples are stored raw in the sender's Unity convention (left-handed, y-up,
// meters, euler degrees); XrNetworkTracker converts to OpenXR space at read
// time. A background thread owns the socket; readers take a short mutex.
//
// Ini: [input] networkTrackers=true / networkTrackerPort=9000

#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>

struct NetTrackerSample {
	bool everSeen = false;
	bool positionSeen = false;
	bool rotationSeen = false;
	// Raw as sent (Unity space): meters / euler degrees / meters-per-second
	float pos[3] = {};
	float eulerDeg[3] = {};
	float vel[3] = {}; // finite-difference between position packets
	uint64_t lastUpdateMs = 0; // position packet time, or atomic frame commit time
};

// Atomic pose-frame metadata carried by /tracking/trackers/frame as three
// exactly representable integer floats: source epoch, pose-validity mask and
// policy flags. Tracker/head messages precede the frame message and are staged;
// the frame message commits the complete snapshot under one lock.
//
// poseMask bits 0..7   = tracker 1..8 position valid
// poseMask bits 8..15  = tracker 1..8 rotation valid
// poseMask bit 16      = head position valid
// poseMask bit 17      = head rotation valid
inline constexpr uint32_t NET_TRACKER_POSITION_MASK = 0x000000ffu;
inline constexpr uint32_t NET_TRACKER_ROTATION_MASK = 0x0000ff00u;
inline constexpr uint32_t NET_TRACKER_HEAD_POSITION_BIT = 1u << 16;
inline constexpr uint32_t NET_TRACKER_HEAD_ROTATION_BIT = 1u << 17;
inline constexpr uint32_t NET_TRACKER_POSE_MASK = (1u << 18) - 1u;

enum NetTrackerFrameFlagBits : uint32_t {
	NetTrackerFrame_UseHeadTranslation = 1u << 0,
	NetTrackerFrame_UseHeadHeightScale = 1u << 1,
	NetTrackerFrame_UseHeadYaw = 1u << 2,
	NetTrackerFrame_FollowHmdYaw = 1u << 3,
	NetTrackerFrame_AllowGaitFootRelease = 1u << 4,
	// Direct, temporally continuous 3D camera poses. Live HTCX/Vive trackers
	// bypass the OSC camera arbitration before these flags are consulted.
	NetTrackerFrame_Continuous3D = 1u << 5,
	// Sender head X/Z is exactly its hip/body root, so the receiver may apply
	// the full HMD-minus-head horizontal correction without making the waist
	// counter-move when shoulders/torso jitter.
	NetTrackerFrame_HeadXZIsBodyRoot = 1u << 6,
};

// Both camera paths yield ordinary feet while artificial locomotion is active,
// allowing VRIK to animate a stride instead of dragging continuously measured
// feet over the ground. The per-leg kick classifier can still keep one camera
// foot live. Physical HTCX/Vive poses never reach this policy gate.
inline constexpr bool NetTrackerFrameUsesCameraFootArbitration(uint32_t flags)
{
	return (flags & (NetTrackerFrame_AllowGaitFootRelease
	                    | NetTrackerFrame_Continuous3D)) != 0;
}

// Keep the sender policy intact. For camera FBT, FollowHmdYaw continuously
// solves HMD yaw minus the measured body yaw; when both turn together the
// difference stays constant, and when the camera loses torso yaw the lower
// body still follows the headset instead of remaining at calibration heading.
inline constexpr uint32_t NetTrackerFrameNormalizePolicyFlags(uint32_t flags)
{
	return flags;
}

// This is an explicit sender contract rather than an inference from
// Continuous3D. Older OCU senders used a shoulder-derived head whose X/Z could
// move during a lean, so hard anchoring those frames would move the waist in
// the opposite direction. Unmarked and legacy heads keep the existing EMA.
inline constexpr bool NetTrackerFrameUsesHardHorizontalRootAlignment(uint32_t flags)
{
	return (flags & NetTrackerFrame_HeadXZIsBodyRoot) != 0;
}

static_assert(!NetTrackerFrameUsesCameraFootArbitration(0));
static_assert(NetTrackerFrameUsesCameraFootArbitration(
    NetTrackerFrame_AllowGaitFootRelease));
static_assert(NetTrackerFrameUsesCameraFootArbitration(
    NetTrackerFrame_Continuous3D));
static_assert(NetTrackerFrameUsesCameraFootArbitration(
    NetTrackerFrame_AllowGaitFootRelease | NetTrackerFrame_Continuous3D));
static_assert(NetTrackerFrameNormalizePolicyFlags(
                  NetTrackerFrame_FollowHmdYaw)
    == NetTrackerFrame_FollowHmdYaw);
static_assert(NetTrackerFrameNormalizePolicyFlags(
                  NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_Continuous3D)
    == (NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_Continuous3D));
static_assert(NetTrackerFrameNormalizePolicyFlags(
                  NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_AllowGaitFootRelease
                  | NetTrackerFrame_Continuous3D)
    == (NetTrackerFrame_FollowHmdYaw | NetTrackerFrame_AllowGaitFootRelease
        | NetTrackerFrame_Continuous3D));
static_assert(!NetTrackerFrameUsesHardHorizontalRootAlignment(0));
static_assert(!NetTrackerFrameUsesHardHorizontalRootAlignment(
    NetTrackerFrame_AllowGaitFootRelease));
static_assert(!NetTrackerFrameUsesHardHorizontalRootAlignment(
    NetTrackerFrame_Continuous3D));
static_assert(NetTrackerFrameUsesHardHorizontalRootAlignment(
    NetTrackerFrame_Continuous3D | NetTrackerFrame_HeadXZIsBodyRoot));

struct NetTrackerFrameSample {
	bool everSeen = false;
	uint32_t sourceEpoch = 0;
	uint32_t poseMask = 0;
	uint32_t flags = 0;
	// Monotonic receiver-side commit id. Gait samples carry the same value,
	// allowing readers to reject a new semantic packet paired with an old pose.
	uint64_t generation = 0;
	uint64_t lastUpdateMs = 0;
};

// Camera-skeleton gait signal. armPhase is a signed, centered left-vs-right
// arm movement in sender meters; handY values are floor-relative meters.
struct NetSkeletonArmsSample {
	bool everSeen = false;
	float armPhase = 0.0f;
	float handY[2] = {};
	uint32_t sourceEpoch = 0;
	uint64_t frameGeneration = 0;
	uint64_t lastUpdateMs = 0;
};

// Semantic camera-leg states carried by /tracking/gait/leg/{left,right}.
// Values are part of the OSC wire contract; do not reorder them.
enum class NetCameraLegState : uint8_t {
	Invalid = 0,
	Grounded = 1,
	UndecidedLift = 2,
	Chamber = 3,
	KneeHold = 4,
	WalkStep = 5,
	KickExtend = 6,
	Recover = 7,
};

struct NetCameraLegSample {
	bool everSeen = false;
	NetCameraLegState state = NetCameraLegState::Invalid;
	float kneeAngleDeg = -1.0f;
	float confidence = 0.0f;
	uint32_t sourceEpoch = 0;
	uint64_t frameGeneration = 0;
	uint64_t lastUpdateMs = 0;
};

class NetworkTrackerReceiver {
public:
	static NetworkTrackerReceiver& Instance();

	static constexpr int MAX_TRACKERS = 8;

	// Idempotent; returns false if the socket could not be created/bound.
	bool Start(int port);
	// Signals the thread, closes the socket and joins (bounded wait).
	void Stop();

	// idx is 0-based (OSC addresses are 1-based). Returns false if never seen.
	// frameOut, when provided, is copied under the same lock as the tracker so
	// readers cannot combine different atomic frame commits.
	bool GetTracker(int idx, NetTrackerSample& out,
	    NetTrackerFrameSample* frameOut = nullptr);
	bool GetHead(NetTrackerSample& out);
	// Returns true only while the last atomic frame commit is within maxAgeMs.
	// When a framed sender was seen, out is populated even if it has gone stale.
	bool GetTrackerFrame(NetTrackerFrameSample& out, uint64_t maxAgeMs);
	bool GetSkeletonArms(NetSkeletonArmsSample& out);
	// side: 0 = left, 1 = right. Returns false when unseen or older than maxAgeMs.
	// When the side has been seen, out is populated even if the sample is stale.
	bool GetCameraLeg(int side, NetCameraLegSample& out, uint64_t maxAgeMs);

	// Playspace alignment: called once per frame pump with the real HMD
	// position/direction. The head pair owns root translation; OCU camera
	// packets also rotate that body frame with HMD yaw. Controller pointers are
	// retained for source compatibility but never drag the swinging skeleton.
	void UpdateAlignment(const float hmdPosStanding[3], const float hmdForwardStanding[2],
	    const float* lctrl, const float* rctrl);
	// Offset to ADD to a z-flipped (already right-handed) sender position,
	// AFTER multiplying by GetAlignmentScale(). Returns false (and zeros/1.0)
	// when no head data has been received, in which case positions are used
	// as-is and the sender is assumed pre-aligned.
	bool GetAlignmentOffset(float out[3]);
	// Camera/OSC sender frame -> standing-space yaw, in radians. Identity when
	// the sender does not provide a head rotation reference.
	float GetAlignmentYaw();

	// Auto height calibration: real-HMD-height / sender-head-height (EMA'd).
	// Corrects the sender's guess of the user's height, so camera senders
	// don't need an accurate height setting.
	float GetAlignmentScale();
	// Framed poses that require alignment stay invalid until the matching
	// epoch has acquired every requested alignment component.
	bool IsAlignmentReady(uint32_t sourceEpoch);

	static uint64_t NowMs();

#ifdef OCU_RUNTIME_SELF_TEST
	// Deterministic in-process hooks used only by the focused runtime semantics
	// test executable.  Production builds never expose or compile these entry
	// points; the test drives the exact staging/commit implementation without a
	// UDP thread or timing sleeps.
	void TestReset();
	void TestStoreTrackerPosition(int idx, float x, float y, float z);
	void TestStoreSkeletonArms(float phase, float leftHandY, float rightHandY);
	void TestStoreCameraLeg(int side, NetCameraLegState state,
	    float kneeAngleDeg, float confidence);
	void TestCommitFrame(uint32_t sourceEpoch, uint32_t poseMask, uint32_t flags);
	void TestParseRawMediaPipePacket(const char* data, int len)
	{
		ParseRawMediaPipePacket(data, len);
	}
#endif

private:
	NetworkTrackerReceiver() = default;
	~NetworkTrackerReceiver();

	void ThreadLoop();
	void ParsePacket(const char* data, int len, int depth);
	void ParseRawMediaPipePacket(const char* data, int len);
	void ParseMessage(const char* data, int len);
	void StoreSample(int slot, bool isRotation, const float xyz[3]);
	void StoreTrackerFrame(const float values[3]);
	void StoreSkeletonArms(const float xyz[3]);
	void StoreCameraLeg(int side, const float values[3]);

	std::mutex mutex; // guards all sample/alignment state below
	std::thread thread;

	NetTrackerSample trackers[MAX_TRACKERS];
	NetTrackerSample head;
	// A framed sender writes here until /tracking/trackers/frame atomically
	// publishes the complete pose. Legacy no-header senders still write the
	// active samples above directly.
	NetTrackerSample pendingTrackers[MAX_TRACKERS];
	NetTrackerSample pendingHead;
	NetTrackerFrameSample trackerFrame;
	NetTrackerSample lhand; // sender wrists: alignment references only,
	NetTrackerSample rhand; // never exposed as tracker devices
	NetSkeletonArmsSample skeletonArms;
	NetSkeletonArmsSample pendingSkeletonArms;
	NetCameraLegSample cameraLegs[2];
	NetCameraLegSample pendingCameraLegs[2];

	// State for the direct OCU3 MediaPipe transport. Raw landmarks cross the
	// process boundary untouched; these filters operate after the one native
	// coordinate conversion and before virtual tracker publication.
	bool rawEpochValid = false;
	uint32_t rawEpoch = 0;
	uint64_t rawSequence = 0;
	uint64_t rawTimestampMs = 0;
	bool rawFloorValid = false;
	float rawFloorY = 0.0f;
	// Per-side standing thigh+shin reach learned only from low, straight legs.
	// Monocular depth commonly foreshortens a raised kick, so kick frames must
	// not redefine the length that the VRIK foot target is expected to reach.
	bool rawStandingLegReachValid[2] = {};
	float rawStandingLegReach[2] = {};
	// Per-side resting ankle height above the heel/toe ground point. The ankle
	// landmark sits 8-12 cm above the sole, so any lift gate measured against
	// the sole-level floor reads a planted foot as permanently lifted unless
	// this offset is subtracted.
	bool rawAnkleOffsetValid[2] = {};
	float rawAnkleOffset[2] = {};
	bool rawFilterValid[9] = {}; // head root + eight body-relative trackers
	float rawFiltered[9][3] = {};
	bool rawYawValid = false;
	float rawYawDeg = 0.0f;

	bool alignValid = false;
	float alignOffset[3] = {};
	float alignScale = 1.0f;
	bool alignScaleValid = false;
	bool alignYawValid = false;
	float alignYawRad = 0.0f;

	std::atomic<uintptr_t> sock{ ~(uintptr_t)0 }; // INVALID_SOCKET without pulling winsock into the header
	int boundPort = 0;
	std::atomic<bool> running{ false };
	bool loggedFirstPacket = false;
};
