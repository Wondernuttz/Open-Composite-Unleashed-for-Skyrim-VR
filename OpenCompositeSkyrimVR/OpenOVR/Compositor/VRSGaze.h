#pragma once

#include <algorithm>
#include <cmath>

namespace ocu_vrs_gaze {

struct Center {
	float x = 0.5f;
	float y = 0.5f;
};

struct Direction {
	float x = 0.0f;
	float y = 0.0f;
	float z = -1.0f;
};

enum class Mode {
	Off,
	Fixed,
	EyeTracked
};

// Auto uses gaze only when a valid sample exists. Fixed is never selected
// implicitly; it must be enabled independently by the user.
inline Mode SelectMode(bool eyeTrackingAuto, bool fixedEnabled, bool gazeValid, bool menuOpen)
{
	if (menuOpen)
		return Mode::Off;
	if (eyeTrackingAuto && gazeValid)
		return Mode::EyeTracked;
	if (fixedEnabled)
		return Mode::Fixed;
	return Mode::Off;
}

// Convert a shared VIEW-space gaze direction into one eye's local view space.
// XrView::pose.orientation maps eye-local coordinates into the space supplied
// to xrLocateViews, so the inverse (the normalized quaternion conjugate) is
// required here. This is identity on parallel headsets and accounts for canted
// views without any vendor-specific calibration offset.
inline bool ToEyeLocal(float dirX, float dirY, float dirZ,
    float eyeQx, float eyeQy, float eyeQz, float eyeQw,
    Direction& out)
{
	if (!std::isfinite(dirX) || !std::isfinite(dirY) || !std::isfinite(dirZ) ||
	    !std::isfinite(eyeQx) || !std::isfinite(eyeQy) ||
	    !std::isfinite(eyeQz) || !std::isfinite(eyeQw))
		return false;

	const float dirLengthSq = dirX * dirX + dirY * dirY + dirZ * dirZ;
	const float quatLengthSq = eyeQx * eyeQx + eyeQy * eyeQy + eyeQz * eyeQz + eyeQw * eyeQw;
	if (dirLengthSq < 0.0001f || quatLengthSq < 0.0001f)
		return false;

	const float invDirLength = 1.0f / std::sqrt(dirLengthSq);
	const float invQuatLength = 1.0f / std::sqrt(quatLengthSq);
	const float vx = dirX * invDirLength;
	const float vy = dirY * invDirLength;
	const float vz = dirZ * invDirLength;
	// Conjugate of the normalized eye-to-VIEW orientation.
	const float qx = -eyeQx * invQuatLength;
	const float qy = -eyeQy * invQuatLength;
	const float qz = -eyeQz * invQuatLength;
	const float qw = eyeQw * invQuatLength;

	// Rotate v by q using q*v*q^-1, expanded to avoid another math dependency.
	const float dot = qx * vx + qy * vy + qz * vz;
	const float crossX = qy * vz - qz * vy;
	const float crossY = qz * vx - qx * vz;
	const float crossZ = qx * vy - qy * vx;
	out.x = 2.0f * dot * qx + (qw * qw - qx * qx - qy * qy - qz * qz) * vx + 2.0f * qw * crossX;
	out.y = 2.0f * dot * qy + (qw * qw - qx * qx - qy * qy - qz * qz) * vy + 2.0f * qw * crossY;
	out.z = 2.0f * dot * qz + (qw * qw - qx * qx - qy * qy - qz * qz) * vz + 2.0f * qw * crossZ;

	return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z) && out.z < -0.01f;
}

// Convert a VIEW-space fixation point into a ray from one eye. Unlike a pure
// direction transform this also accounts for IPD and the gaze pose's origin.
inline bool ToEyeLocalPoint(float pointX, float pointY, float pointZ,
    float eyeX, float eyeY, float eyeZ,
    float eyeQx, float eyeQy, float eyeQz, float eyeQw,
    Direction& out)
{
	if (!std::isfinite(pointX) || !std::isfinite(pointY) || !std::isfinite(pointZ) ||
	    !std::isfinite(eyeX) || !std::isfinite(eyeY) || !std::isfinite(eyeZ))
		return false;
	return ToEyeLocal(pointX - eyeX, pointY - eyeY, pointZ - eyeZ,
	    eyeQx, eyeQy, eyeQz, eyeQw, out);
}

// Project an eye-local gaze direction using that eye's OpenXR FOV tangents.
// OpenXR looks down -Z and texture V grows down.
inline bool Project(float dirX, float dirY, float dirZ,
    float tanLeft, float tanRight, float tanUp, float tanDown,
    Center& out)
{
	if (!std::isfinite(dirX) || !std::isfinite(dirY) || !std::isfinite(dirZ) ||
	    !std::isfinite(tanLeft) || !std::isfinite(tanRight) ||
	    !std::isfinite(tanUp) || !std::isfinite(tanDown) || dirZ >= -0.01f)
		return false;

	const float spanX = tanRight - tanLeft;
	const float spanY = tanUp - tanDown;
	if (spanX <= 0.001f || spanY <= 0.001f)
		return false;

	const float tanX = dirX / -dirZ;
	const float tanY = dirY / -dirZ;
	const float x = (tanX - tanLeft) / spanX;
	const float y = (tanUp - tanY) / spanY;
	if (!std::isfinite(x) || !std::isfinite(y))
		return false;

	// Looking beyond the visible eye still needs a useful edge fovea. A small
	// inset keeps the full-rate ring on the shading-rate image instead of mostly
	// outside it, while normal in-FOV samples remain untouched.
	out.x = std::clamp(x, 0.02f, 0.98f);
	out.y = std::clamp(y, 0.02f, 0.98f);
	return true;
}

inline bool ProjectViewSpace(float dirX, float dirY, float dirZ,
    float eyeQx, float eyeQy, float eyeQz, float eyeQw,
    float tanLeft, float tanRight, float tanUp, float tanDown,
    Center& out, Direction* eyeLocalOut = nullptr)
{
	Direction eyeLocal;
	if (!ToEyeLocal(dirX, dirY, dirZ, eyeQx, eyeQy, eyeQz, eyeQw, eyeLocal))
		return false;
	if (eyeLocalOut)
		*eyeLocalOut = eyeLocal;
	return Project(eyeLocal.x, eyeLocal.y, eyeLocal.z,
	    tanLeft, tanRight, tanUp, tanDown, out);
}

inline bool ProjectViewSpacePoint(float pointX, float pointY, float pointZ,
    float eyeX, float eyeY, float eyeZ,
    float eyeQx, float eyeQy, float eyeQz, float eyeQw,
    float tanLeft, float tanRight, float tanUp, float tanDown,
    Center& out, Direction* eyeLocalOut = nullptr)
{
	Direction eyeLocal;
	if (!ToEyeLocalPoint(pointX, pointY, pointZ,
	        eyeX, eyeY, eyeZ, eyeQx, eyeQy, eyeQz, eyeQw, eyeLocal))
		return false;
	if (eyeLocalOut)
		*eyeLocalOut = eyeLocal;
	return Project(eyeLocal.x, eyeLocal.y, eyeLocal.z,
	    tanLeft, tanRight, tanUp, tanDown, out);
}

inline Center Smooth(const Center& previous, const Center& target, float dtSeconds,
    bool hasPrevious, float cutoffHz = 30.0f)
{
	if (!hasPrevious || !std::isfinite(dtSeconds) || dtSeconds <= 0.0f || dtSeconds > 0.1f)
		return target;
	const float dx = target.x - previous.x;
	const float dy = target.y - previous.y;
	const float distance = std::hypot(dx, dy);
	// Saccades follow the current sample; filtering small tracker noise may
	// leave at most 0.2% of an eye texture between the gaze and ring center.
	constexpr float snapDistance = 0.03f;
	constexpr float maxLag = 0.002f;
	if (!std::isfinite(distance) || distance >= snapDistance)
		return target;
	const float residual = std::exp(-2.0f * 3.14159265358979323846f * cutoffHz * dtSeconds);
	const float boundedResidual = distance > maxLag ? std::min(residual, maxLag / distance) : residual;
	return { target.x - boundedResidual * dx, target.y - boundedResidual * dy };
}

} // namespace ocu_vrs_gaze
