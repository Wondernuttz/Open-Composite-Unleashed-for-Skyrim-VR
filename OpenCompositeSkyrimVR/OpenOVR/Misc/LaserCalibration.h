#pragma once

#include "Misc/Config.h"
#include "Misc/xrutil.h"

#include <cmath>

namespace oovr_laser_calibration {

constexpr float kPi = 3.14159265358979323846f;

inline bool Enabled(int side)
{
	return side == 0
	    ? oovr_global_configuration.AdjustLeftLaserRotation()
	    : oovr_global_configuration.AdjustRightLaserRotation();
}

inline void Angles(int side, float& x, float& y, float& z)
{
	if (side == 0) {
		x = oovr_global_configuration.LeftLaserXRotation();
		y = oovr_global_configuration.LeftLaserYRotation();
		z = oovr_global_configuration.LeftLaserZRotation();
	} else {
		x = oovr_global_configuration.RightLaserXRotation();
		y = oovr_global_configuration.RightLaserYRotation();
		z = oovr_global_configuration.RightLaserZRotation();
	}
}

inline XrVector3f RotateLocalVector(XrVector3f v, float xDeg, float yDeg, float zDeg)
{
	const float rx = xDeg * kPi / 180.0f;
	const float ry = yDeg * kPi / 180.0f;
	const float rz = zDeg * kPi / 180.0f;

	const float sx = sinf(rx);
	const float cx = cosf(rx);
	const float sy = sinf(ry);
	const float cy = cosf(ry);
	const float sz = sinf(rz);
	const float cz = cosf(rz);

	XrVector3f out = {
		v.x,
		v.y * cx - v.z * sx,
		v.y * sx + v.z * cx
	};
	v = {
		out.x * cy + out.z * sy,
		out.y,
		-out.x * sy + out.z * cy
	};
	out = {
		v.x * cz - v.y * sz,
		v.x * sz + v.y * cz,
		v.z
	};
	return out;
}

inline XrVector3f LocalForward(int side)
{
	XrVector3f fwd{ 0.0f, 0.0f, -1.0f };
	if (!Enabled(side))
		return fwd;

	float x, y, z;
	Angles(side, x, y, z);
	return RotateLocalVector(fwd, x, y, z);
}

// Beam-origin correction in meters along the controller aim pose's local -Y.
// This moves the rendered beam and the hit-test ray together.
inline float OriginDown(int side)
{
	return side == 0
	    ? oovr_global_configuration.LeftLaserOriginDown()
	    : oovr_global_configuration.RightLaserOriginDown();
}

inline XrVector3f TransformVector(const vr::HmdMatrix34_t& m, const XrVector3f& v)
{
	return {
		m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z,
		m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z,
		m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z
	};
}

inline void ApplyToPoseMatrix(int side, vr::HmdMatrix34_t& m)
{
	if (!Enabled(side))
		return;

	float x, y, z;
	Angles(side, x, y, z);

	const XrVector3f col0 = TransformVector(m, RotateLocalVector({ 1.0f, 0.0f, 0.0f }, x, y, z));
	const XrVector3f col1 = TransformVector(m, RotateLocalVector({ 0.0f, 1.0f, 0.0f }, x, y, z));
	const XrVector3f col2 = TransformVector(m, RotateLocalVector({ 0.0f, 0.0f, 1.0f }, x, y, z));

	m.m[0][0] = col0.x; m.m[1][0] = col0.y; m.m[2][0] = col0.z;
	m.m[0][1] = col1.x; m.m[1][1] = col1.y; m.m[2][1] = col1.z;
	m.m[0][2] = col2.x; m.m[1][2] = col2.y; m.m[2][2] = col2.z;
}

}
