#pragma once

#include <cmath>
#include <openxr/openxr.h>

namespace OcuBodyTrackerPose {

struct Sample {
	bool valid = false;
	bool tracked = false;
	XrPosef pose{};
	XrVector3f linearVelocity{};
	XrVector3f angularVelocity{};
};

inline bool IsFinite(const XrVector3f& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// A tracker publishes a full rigid transform. A position-only result must not
// become a valid OpenVR pose or prevent another tracking source from taking over.
inline Sample FromLocation(XrResult result, const XrSpaceLocation& location,
    const XrSpaceVelocity& velocity)
{
	Sample sample;
	constexpr XrSpaceLocationFlags required =
	    XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if (XR_FAILED(result) || (location.locationFlags & required) != required
	    || !IsFinite(location.pose.position))
		return sample;

	const XrQuaternionf& orientation = location.pose.orientation;
	if (!std::isfinite(orientation.x) || !std::isfinite(orientation.y)
	    || !std::isfinite(orientation.z) || !std::isfinite(orientation.w))
		return sample;

	const double lengthSquared = double(orientation.x) * orientation.x
	    + double(orientation.y) * orientation.y + double(orientation.z) * orientation.z
	    + double(orientation.w) * orientation.w;
	// Runtime orientations are unit quaternions. Tolerate small numeric drift,
	// but do not turn a corrupt or degenerate quaternion into a plausible pose.
	if (lengthSquared < 0.99 || lengthSquared > 1.01)
		return sample;

	sample.pose = location.pose;
	const float inverseLength = static_cast<float>(1.0 / std::sqrt(lengthSquared));
	sample.pose.orientation.x *= inverseLength;
	sample.pose.orientation.y *= inverseLength;
	sample.pose.orientation.z *= inverseLength;
	sample.pose.orientation.w *= inverseLength;
	if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0
	    && IsFinite(velocity.linearVelocity))
		sample.linearVelocity = velocity.linearVelocity;
	if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0
	    && IsFinite(velocity.angularVelocity))
		sample.angularVelocity = velocity.angularVelocity;
	// VALID without TRACKED is an inferred runtime pose, which is still usable.
	sample.valid = true;
	constexpr XrSpaceLocationFlags tracked =
	    XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	sample.tracked = (location.locationFlags & tracked) == tracked;
	return sample;
}

} // namespace OcuBodyTrackerPose
