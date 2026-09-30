#include "stdafx.h"

#include "LaserRaySmoothing.h"

#include "Misc/Config.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>

namespace oovr_laser_smoothing {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMaxGapSeconds = 0.25f;
constexpr float kPositionSnapMeters = 0.35f;
constexpr float kDirectionSnapRadians = 60.0f * kPi / 180.0f;

struct Key {
	Consumer consumer;
	int side;
	XrSpace referenceSpace;
};

struct KeyLess {
	bool operator()(const Key& a, const Key& b) const
	{
		if (a.consumer != b.consumer)
			return static_cast<int>(a.consumer) < static_cast<int>(b.consumer);
		if (a.side != b.side)
			return a.side < b.side;
		return std::less<XrSpace>{}(a.referenceSpace, b.referenceSpace);
	}
};

struct State {
	XrTime sampleTime = 0;
	std::chrono::steady_clock::time_point wallTime{};
	XrVector3f lastRawOrigin{};
	XrVector3f lastRawDirection{};
	XrVector3f filteredOrigin{};
	XrVector3f filteredDirection{};
	float filteredPositionSpeed = 0.0f;
	float filteredAngularSpeed = 0.0f;
};

std::map<Key, State, KeyLess> states;
std::mutex statesMutex;

bool IsFinite(const XrVector3f& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float Dot(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Distance(const XrVector3f& a, const XrVector3f& b)
{
	const float x = a.x - b.x;
	const float y = a.y - b.y;
	const float z = a.z - b.z;
	return std::sqrt(x * x + y * y + z * z);
}

bool Normalize(XrVector3f& value)
{
	if (!IsFinite(value))
		return false;
	const float lengthSquared = Dot(value, value);
	if (!std::isfinite(lengthSquared) || lengthSquared < 1e-8f)
		return false;
	const float inverseLength = 1.0f / std::sqrt(lengthSquared);
	value.x *= inverseLength;
	value.y *= inverseLength;
	value.z *= inverseLength;
	return IsFinite(value);
}

float DirectionAngle(const XrVector3f& a, const XrVector3f& b)
{
	return std::acos(std::clamp(Dot(a, b), -1.0f, 1.0f));
}

float Alpha(float cutoffHz, float dt)
{
	cutoffHz = std::max(cutoffHz, 0.01f);
	return 1.0f - std::exp(-2.0f * kPi * cutoffHz * dt);
}

float SafeSetting(float value, float minimum, float maximum, float fallback)
{
	return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

XrVector3f Lerp(const XrVector3f& from, const XrVector3f& to, float alpha)
{
	return {
		from.x + (to.x - from.x) * alpha,
		from.y + (to.y - from.y) * alpha,
		from.z + (to.z - from.z) * alpha
	};
}

void Initialize(State& state, XrTime sampleTime,
    std::chrono::steady_clock::time_point wallTime,
    const XrVector3f& origin, const XrVector3f& direction)
{
	state.sampleTime = sampleTime;
	state.wallTime = wallTime;
	state.lastRawOrigin = state.filteredOrigin = origin;
	state.lastRawDirection = state.filteredDirection = direction;
	state.filteredPositionSpeed = 0.0f;
	state.filteredAngularSpeed = 0.0f;
}

} // namespace

bool Filter(Consumer consumer, int side, XrSpace referenceSpace, XrTime sampleTime,
    XrVector3f& origin, XrVector3f& direction)
{
	const Key key{ consumer, side, referenceSpace };
	const float directionLengthSquared = Dot(direction, direction);
	if (side < 0 || side > 1 || referenceSpace == XR_NULL_HANDLE ||
	    !IsFinite(origin) || !IsFinite(direction) ||
	    !std::isfinite(directionLengthSquared) || directionLengthSquared < 1e-8f) {
		std::lock_guard<std::mutex> lock(statesMutex);
		states.erase(key);
		return false;
	}

	std::lock_guard<std::mutex> lock(statesMutex);
	// Direction normalization is a ray invariant, not smoothing. Keep it even
	// when filtering is disabled so beam and hit distances match the prior
	// UpdateWorld path and cannot be scaled by a malformed runtime quaternion.
	Normalize(direction);
	if (!oovr_global_configuration.EnableLaserSmoothing()) {
		states.erase(key);
		return true;
	}

	const auto now = std::chrono::steady_clock::now();
	auto stateIt = states.find(key);
	if (stateIt == states.end()) {
		State& fresh = states[key];
		Initialize(fresh, sampleTime, now, origin, direction);
		return true;
	}

	State& state = stateIt->second;
	if (sampleTime != 0 && sampleTime == state.sampleTime) {
		origin = state.filteredOrigin;
		direction = state.filteredDirection;
		return true;
	}

	const float wallDt = std::chrono::duration<float>(now - state.wallTime).count();
	float dt = wallDt;
	if (sampleTime > state.sampleTime && state.sampleTime != 0)
		dt = static_cast<float>(sampleTime - state.sampleTime) / 1000000000.0f;

	const float positionDelta = Distance(origin, state.lastRawOrigin);
	const float directionDelta = DirectionAngle(direction, state.lastRawDirection);
	const bool reset = !std::isfinite(dt) || dt <= 0.0f || wallDt > kMaxGapSeconds ||
	    dt > kMaxGapSeconds || sampleTime < state.sampleTime ||
	    positionDelta > kPositionSnapMeters || directionDelta > kDirectionSnapRadians;
	if (reset) {
		Initialize(state, sampleTime, now, origin, direction);
		return true;
	}

	const float derivativeAlpha = Alpha(1.0f, dt);
	state.filteredPositionSpeed +=
	    (positionDelta / dt - state.filteredPositionSpeed) * derivativeAlpha;
	state.filteredAngularSpeed +=
	    (directionDelta / dt - state.filteredAngularSpeed) * derivativeAlpha;
	const float positionMinCutoff = SafeSetting(
	    oovr_global_configuration.LaserPosSmoothMinCutoff(), 0.01f, 20.0f, 6.0f);
	const float positionBeta = SafeSetting(
	    oovr_global_configuration.LaserPosSmoothBeta(), 0.0f, 100.0f, 12.0f);
	const float directionMinCutoff = SafeSetting(
	    oovr_global_configuration.LaserRotSmoothMinCutoff(), 0.01f, 20.0f, 4.0f);
	const float directionBeta = SafeSetting(
	    oovr_global_configuration.LaserRotSmoothBeta(), 0.0f, 10.0f, 0.35f);
	const float positionCutoff = positionMinCutoff +
	    positionBeta * state.filteredPositionSpeed;
	const float directionCutoff = directionMinCutoff +
	    directionBeta * state.filteredAngularSpeed;

	state.filteredOrigin = Lerp(state.filteredOrigin, origin, Alpha(positionCutoff, dt));
	state.filteredDirection = Lerp(
	    state.filteredDirection, direction, Alpha(directionCutoff, dt));
	if (!Normalize(state.filteredDirection) || !IsFinite(state.filteredOrigin)) {
		states.erase(stateIt);
		return false;
	}

	state.sampleTime = sampleTime;
	state.wallTime = now;
	state.lastRawOrigin = origin;
	state.lastRawDirection = direction;
	origin = state.filteredOrigin;
	direction = state.filteredDirection;
	return true;
}

void Reset(Consumer consumer, int side, XrSpace referenceSpace)
{
	std::lock_guard<std::mutex> lock(statesMutex);
	states.erase(Key{ consumer, side, referenceSpace });
}

void ResetAll()
{
	{
		std::lock_guard<std::mutex> lock(statesMutex);
		states.clear();
	}
	OOVR_LOG("Laser smoothing: reset all ray filters for OpenXR session replacement");
}

}
