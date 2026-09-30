#include "DrvOpenXR/BodyTrackerPose.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "OpenOVR/Misc/BodyTrackerStatus.h"

#include <iostream>
#include <limits>

namespace {

XrSpaceLocation ValidLocation()
{
	XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
	location.locationFlags =
	    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	location.pose.position = { 1.25f, -0.75f, 2.5f };
	location.pose.orientation = { 0.0f, 0.0f, 0.6f, 0.8f };
	return location;
}

bool Equal(const XrVector3f& a, const XrVector3f& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool Empty(const OcuBodyTrackerPose::Sample& sample)
{
	return !sample.valid && !sample.tracked && Equal(sample.pose.position, {})
	    && sample.pose.orientation.x == 0.0f && sample.pose.orientation.y == 0.0f
	    && sample.pose.orientation.z == 0.0f && sample.pose.orientation.w == 0.0f
	    && Equal(sample.linearVelocity, {}) && Equal(sample.angularVelocity, {});
}

bool Near(float a, float b)
{
	return std::abs(a - b) < 1e-6f;
}

} // namespace

int main()
{
	int failures = 0;
	auto check = [&](bool condition, const char* name) {
		if (!condition) {
			std::cerr << "FAIL: " << name << '\n';
			++failures;
		}
	};
	XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
	velocity.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
	velocity.linearVelocity = { -1.0f, 2.0f, 3.0f };
	velocity.angularVelocity = { 4.0f, -5.0f, 6.0f };
	const auto validLocation = ValidLocation();
	const auto inferred = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, validLocation, velocity);
	check(inferred.valid, "inferred pose remains valid without either TRACKED flag");
	check(!inferred.tracked, "inferred pose is distinguished from actual runtime tracking");
	check(Equal(inferred.pose.position, validLocation.pose.position), "runtime position preserved without offsets or filtering");
	check(Near(inferred.pose.orientation.z, 0.6f) && Near(inferred.pose.orientation.w, 0.8f), "runtime rotation preserved");
	check(Equal(inferred.linearVelocity, velocity.linearVelocity)
	        && Equal(inferred.angularVelocity, velocity.angularVelocity),
	    "valid runtime velocities preserve signs, axes and magnitudes");

	for (const auto tracked : { XrSpaceLocationFlags{ 0 },
	         XR_SPACE_LOCATION_POSITION_TRACKED_BIT, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT,
	         XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT }) {
		auto location = validLocation;
		location.locationFlags |= tracked;
		const auto sample = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity);
		check(sample.valid, "TRACKED flags do not impose an extra validity requirement");
		check(sample.tracked == (tracked == (XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)),
		    "actual tracking requires both TRACKED flags");
		for (const auto missing : { XR_SPACE_LOCATION_POSITION_VALID_BIT, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT }) {
			location.locationFlags = (validLocation.locationFlags & ~missing) | tracked;
			check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity)), "either missing VALID flag rejects the entire pose, even if TRACKED");
		}
	}
	check(Empty(OcuBodyTrackerPose::FromLocation(XR_ERROR_RUNTIME_FAILURE, validLocation, velocity)), "failed locate cannot publish otherwise-valid output");
	XrSpaceLocation unavailable{ XR_TYPE_SPACE_LOCATION };
	check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, unavailable, velocity)), "unavailable pose clears transform and velocities");

	for (const float invalid : { std::numeric_limits<float>::quiet_NaN(),
	         std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() }) {
		for (int component = 0; component < 3; ++component) {
			auto location = validLocation;
			float* position[] = { &location.pose.position.x, &location.pose.position.y, &location.pose.position.z };
			*position[component] = invalid;
			check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity)), "non-finite position is rejected");
		}
		for (int component = 0; component < 4; ++component) {
			auto location = validLocation;
			float* orientation[] = { &location.pose.orientation.x, &location.pose.orientation.y,
				&location.pose.orientation.z, &location.pose.orientation.w };
			*orientation[component] = invalid;
			check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity)), "non-finite orientation is rejected");
		}
		for (int component = 0; component < 3; ++component) {
			auto badVelocity = velocity;
			float* linear[] = { &badVelocity.linearVelocity.x, &badVelocity.linearVelocity.y, &badVelocity.linearVelocity.z };
			*linear[component] = invalid;
			const auto sample = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, validLocation, badVelocity);
			check(sample.valid && Equal(sample.linearVelocity, {}) && Equal(sample.angularVelocity, velocity.angularVelocity),
			    "non-finite linear velocity is zeroed without dropping pose or angular velocity");
			badVelocity = velocity;
			float* angular[] = { &badVelocity.angularVelocity.x, &badVelocity.angularVelocity.y, &badVelocity.angularVelocity.z };
			*angular[component] = invalid;
			const auto angularSample = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, validLocation, badVelocity);
			check(angularSample.valid && Equal(angularSample.angularVelocity, {}) && Equal(angularSample.linearVelocity, velocity.linearVelocity),
			    "non-finite angular velocity is zeroed without dropping pose or linear velocity");
		}
	}

	for (const float w : { 0.0f, 0.000001f, 0.5f, 2.0f, std::numeric_limits<float>::max() }) {
		auto location = validLocation;
		location.pose.orientation = { 0.0f, 0.0f, 0.0f, w };
		check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity)), "degenerate and unreasonable quaternion lengths are rejected");
	}
	for (const float w : { 1.002f, -1.002f }) {
		auto location = validLocation;
		location.pose.orientation = { 0.0f, 0.0f, 0.0f, w };
		const auto sample = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, location, velocity);
		check(sample.valid && Near(sample.pose.orientation.w, w > 0 ? 1.0f : -1.0f), "small quaternion drift normalized without changing its sign");
	}

	for (const auto flags : { XrSpaceVelocityFlags{ 0 }, XR_SPACE_VELOCITY_LINEAR_VALID_BIT,
	         XR_SPACE_VELOCITY_ANGULAR_VALID_BIT,
	         XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT }) {
		auto maskedVelocity = velocity;
		maskedVelocity.velocityFlags = flags;
		const auto sample = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, validLocation, maskedVelocity);
		check(sample.valid, "missing velocity flags do not invalidate the pose");
		check(Equal(sample.linearVelocity, (flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) ? velocity.linearVelocity : XrVector3f{}),
		    "linear velocity requires its own VALID flag");
		check(Equal(sample.angularVelocity, (flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) ? velocity.angularVelocity : XrVector3f{}),
		    "angular velocity requires its own VALID flag");
	}
	// No state survives a read: invalidation and recovery cannot leak an old
	// role's transform or velocity into a replacement session or another role.
	check(Empty(OcuBodyTrackerPose::FromLocation(XR_SUCCESS, unavailable, velocity)), "valid-to-invalid transition clears the sample");
	const auto recovered = OcuBodyTrackerPose::FromLocation(XR_SUCCESS, validLocation, XrSpaceVelocity{ XR_TYPE_SPACE_VELOCITY });
	check(recovered.valid && Equal(recovered.linearVelocity, {}) && Equal(recovered.angularVelocity, {}), "recovered pose has no stale velocity");

	constexpr uint64_t statusTime = 0x123456789ULL;
	for (bool valid : { false, true }) {
		for (bool tracked : { false, true }) {
			const uint64_t packed = BodyTrackerStatus::Pack(statusTime, valid, tracked);
			check((packed >> 2) == statusTime && bool(packed & 1) == valid
			        && bool(packed & 2) == (valid && tracked),
			    "status word preserves its timestamp and never reports invalid tracking as tracked");
		}
	}
#ifdef _WIN32
	check(offsetof(BodyTrackerStatus::Snapshot, roles) == 16
	        && sizeof(BodyTrackerStatus::Snapshot) == 16 + 8 * OCU_TRACKER_ROLE_COUNT
	        && alignof(BodyTrackerStatus::Snapshot) == 8,
	    "native status layout matches the fixed header and aligned 64-bit role contract");
	BodyTrackerStatus::Publish(0, true, true);
	BodyTrackerStatus::Publish(1, true, false);
	BodyTrackerStatus::Publish(2, false, true);
	wchar_t mappingName[80]{};
	swprintf_s(mappingName, L"Local\\OCU.BodyTrackers.v1.%lu", GetCurrentProcessId());
	HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
	check(mapping != nullptr, "publisher creates its process-specific status mapping");
	if (mapping) {
		const auto* view = static_cast<const BodyTrackerStatus::Snapshot*>(
		    MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(BodyTrackerStatus::Snapshot)));
		check(view != nullptr, "status mapping can be opened by a read-only consumer");
		if (view) {
			check(view->magic == BodyTrackerStatus::Magic && view->version == BodyTrackerStatus::Version
			        && view->processId == GetCurrentProcessId() && view->roleCount == OCU_TRACKER_ROLE_COUNT,
			    "native publisher writes the versioned header expected by readers");
			const uint64_t tracked = static_cast<uint64_t>(view->roles[0]);
			const uint64_t inferred = static_cast<uint64_t>(view->roles[1]);
			const uint64_t invalid = static_cast<uint64_t>(view->roles[2]);
			check((tracked & 3) == 3 && (inferred & 3) == 1 && (invalid & 3) == 0,
			    "mapping distinguishes tracked, inferred, and invalid pose observations");
			const uint64_t observedNow = GetTickCount64();
			check((tracked >> 2) <= observedNow && observedNow - (tracked >> 2) < 5000,
			    "published timestamp uses the reader-compatible monotonic millisecond clock");
			BodyTrackerStatus::Publish(-1, false, false);
			BodyTrackerStatus::Publish(OCU_TRACKER_ROLE_COUNT, false, false);
			check(static_cast<uint64_t>(view->roles[0]) == tracked
			        && static_cast<uint64_t>(view->roles[1]) == inferred,
			    "out-of-range publication leaves existing roles and header untouched");
			UnmapViewOfFile(view);
		}
		CloseHandle(mapping);
	}
#endif

	if (failures)
		return 1;
	std::cout << "PASS: body tracker location validity, quaternion sanity, inferred poses, independent velocity validity and native status mapping\n";
	return 0;
}
