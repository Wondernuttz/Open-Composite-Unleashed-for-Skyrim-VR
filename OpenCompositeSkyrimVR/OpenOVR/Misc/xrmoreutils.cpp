//
// Created by ZNix on 15/03/2021.
//

#include "stdafx.h"

#include "Misc/Config.h"
#include "../InputTrace.h"
#include "OneEuroFilterPosition.cpp"
#include "OneEuroFilterRotation.cpp"
#include "xrmoreutils.h"
#include <chrono>
#include <cmath>
#include <convert.h>
#include <map>
#include <mutex>

namespace {

struct ControllerPoseFilterKey {
	int device;
	int origin;

	bool operator<(const ControllerPoseFilterKey& other) const
	{
		return device < other.device || (device == other.device && origin < other.origin);
	}
};

struct ControllerPoseFilterState {
	std::chrono::steady_clock::time_point lastSample{};
	glm::vec3 lastRawPosition{};
	glm::quat lastRawRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	glm::vec3 filteredPosition{};
	glm::quat filteredRotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	bool initialized = false;
};

static std::map<ControllerPoseFilterKey, OneEuroFilterRotation> rotationFilters;
static std::map<ControllerPoseFilterKey, OneEuroFilterPosition> positionFilters;
static std::map<ControllerPoseFilterKey, ControllerPoseFilterState> filterStates;
static std::mutex controllerPoseFilterMutex;

static void ResetControllerPoseFilter(const ControllerPoseFilterKey& key)
{
	rotationFilters.erase(key);
	positionFilters.erase(key);
	filterStates.erase(key);
}

static float QuaternionAngle(const glm::quat& a, const glm::quat& b)
{
	float dot = std::abs(glm::dot(glm::normalize(a), glm::normalize(b)));
	dot = std::clamp(dot, 0.0f, 1.0f);
	return 2.0f * std::acos(dot);
}

static bool IsFinite(const glm::vec3& value)
{
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

static bool IsFinite(const glm::quat& value)
{
	return std::isfinite(value.w) && std::isfinite(value.x) &&
	    std::isfinite(value.y) && std::isfinite(value.z);
}

static float SafeFilterSetting(float value, float minimum, float maximum, float fallback)
{
	return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}

} // namespace

glm::vec3 toEulerAngles(const glm::quat& q)
{
	glm::vec3 angles;

	double sinr_cosp = +2.0 * (q.w * q.x + q.y * q.z);
	double cosr_cosp = +1.0 - 2.0 * (q.x * q.x + q.y * q.y);
	angles.x = atan2(sinr_cosp, cosr_cosp);

	double sinp = +2.0 * (q.w * q.y - q.z * q.x);
	if (fabs(sinp) >= 1)
		angles.y = copysign(M_PI / 2, sinp);
	else
		angles.y = asin(sinp);

	double siny_cosp = +2.0 * (q.w * q.z + q.x * q.y);
	double cosy_cosp = +1.0 - 2.0 * (q.y * q.y + q.z * q.z);
	angles.z = atan2(siny_cosp, cosy_cosp);

	return angles;
}

Quaternion toQuaternion(const glm::quat& q)
{
	return Quaternion(q.x, q.y, q.z, q.w);
}

glm::quat toGLMQuat(const Quaternion& q)
{
	return glm::quat(q.w, q.x, q.y, q.z);
}

void xr_utils::ResetControllerPoseFilters()
{
	{
		std::lock_guard<std::mutex> lock(controllerPoseFilterMutex);
		rotationFilters.clear();
		positionFilters.clear();
		filterStates.clear();
	}
	OOVR_LOG("Controller smoothing: reset filters for OpenXR session/reference-space change");
}

void xr_utils::PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space,
    vr::ETrackingUniverseOrigin origin, std::optional<glm::mat4> extraTransform, int device)
{
	auto baseSpace = xr_space_from_tracking_origin(origin);

	XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation info{ XR_TYPE_SPACE_LOCATION, &velocity, 0, {} };
	const ControllerPoseFilterKey filterKey{ device, static_cast<int>(origin) };
	XrResult locateResult = xrLocateSpace(space, baseSpace, xr_gbl->GetBestTime(), &info);
	// Observe the existing locate result, without doing another tracking query.
	if (extraTransform && device >= 0 && device < 2
	    && (oovr_debug_logging_enabled() || XR_FAILED(locateResult))) {
		thread_local OcuInputTrace::ChangeGate poseTrace[2];
		if (poseTrace[device].Allow(true, { OcuInputTrace::Handle(xr_session.get()), OcuInputTrace::Code(locateResult),
		        info.locationFlags, (uint64_t)origin, OcuInputTrace::Handle(space) }, OcuLogging::NowMs())) {
			OOVR_LOGF("[INPUT-TRACE] Pose session=%p hand=%s space=%p origin=%d result=%s(%d) flags=0x%llx positionValid=%d orientationValid=%d positionTracked=%d orientationTracked=%d",
			    (void*)xr_session.get(), device == 0 ? "left" : "right", (void*)space, (int)origin,
			    OcuInputTrace::Result(locateResult), (int)locateResult, (unsigned long long)info.locationFlags,
			    !!(info.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT), !!(info.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT),
			    !!(info.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT), !!(info.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT));
		}
	}
	if (XR_FAILED(locateResult)) {
		OOVR_FAILED_XR_SOFT_ABORT(locateResult);
		if (extraTransform) {
			std::lock_guard<std::mutex> lock(controllerPoseFilterMutex);
			ResetControllerPoseFilter(filterKey);
		}
		*pose = {};
		pose->bDeviceIsConnected = true;
		pose->eTrackingResult = vr::TrackingResult_Running_OutOfRange;
		return;
	}

	const bool positionValid =
	    (info.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	const bool orientationValid =
	    (info.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	// Preserve OpenComposite's existing position-valid semantics for HMDs,
	// generic trackers, and action-pose queries. The controller transform/filter
	// path consumes orientation, so it must require both flags.
	bool poseValid = positionValid && (!extraTransform || orientationValid);
	if (!poseValid && extraTransform) {
		std::lock_guard<std::mutex> lock(controllerPoseFilterMutex);
		ResetControllerPoseFilter(filterKey);
	}

	glm::mat4 mat = X2G_om34_pose(info.pose);

	if (extraTransform) {
		mat = mat * extraTransform.value();

		glm::vec3 rawPosition = glm::vec3(mat[3]);
		glm::quat rawRotation = glm::quat_cast(mat);
		const float rotationLengthSquared = glm::dot(rawRotation, rawRotation);
		if (poseValid && (!IsFinite(rawPosition) || !IsFinite(rawRotation) ||
		                     !std::isfinite(rotationLengthSquared) || rotationLengthSquared < 1e-8f)) {
			OOVR_LOG_ONCE("Controller smoothing: rejected non-finite or degenerate transformed pose");
			{
				std::lock_guard<std::mutex> lock(controllerPoseFilterMutex);
				ResetControllerPoseFilter(filterKey);
			}
			poseValid = false;
			mat = glm::mat4(1.0f);
		}

		if (poseValid && oovr_global_configuration.EnableControllerSmoothing()) {
			const auto now = std::chrono::steady_clock::now();
			rawRotation = glm::normalize(rawRotation);
			const float positionMinCutoff = SafeFilterSetting(
			    oovr_global_configuration.PosSmoothMinCutoff(), 0.01f, 20.0f, 1.25f);
			const float positionBeta = SafeFilterSetting(
			    oovr_global_configuration.PosSmoothBeta(), 0.0f, 100.0f, 20.0f);
			const float rotationMinCutoff = SafeFilterSetting(
			    oovr_global_configuration.RotSmoothMinCutoff(), 0.01f, 20.0f, 1.5f);
			const float rotationBeta = SafeFilterSetting(
			    oovr_global_configuration.RotSmoothBeta(), 0.0f, 10.0f, 0.2f);
			std::lock_guard<std::mutex> lock(controllerPoseFilterMutex);
			auto stateIt = filterStates.find(filterKey);
			const bool initialized = stateIt != filterStates.end() && stateIt->second.initialized;

			const float elapsed = initialized
			    ? std::chrono::duration<float>(now - stateIt->second.lastSample).count()
			    : 0.0f;
			const bool discontinuity = initialized &&
			    (glm::distance(rawPosition, stateIt->second.lastRawPosition) > 0.35f ||
			        QuaternionAngle(rawRotation, stateIt->second.lastRawRotation) > glm::radians(60.0f));
			const bool stale = initialized && elapsed > 0.25f;

			if (!initialized || discontinuity || stale) {
				// Start directly at the runtime pose after a new session, tracking
				// origin change, interaction-profile swap, or long tracking gap.
				ResetControllerPoseFilter(filterKey);
				ControllerPoseFilterState& fresh = filterStates[filterKey];
				fresh.lastSample = now;
				fresh.lastRawPosition = fresh.filteredPosition = rawPosition;
				fresh.lastRawRotation = fresh.filteredRotation = rawRotation;
				fresh.initialized = true;

				auto posIt = positionFilters.emplace(filterKey, OneEuroFilterPosition(
				    90.0, positionMinCutoff, positionBeta, 1)).first;
				auto rotIt = rotationFilters.emplace(filterKey, OneEuroFilterRotation(
				    90.0f, rotationMinCutoff, rotationBeta, 1)).first;
				posIt->second.filter(rawPosition, glm::vec3(0.0f));
				Quaternion prime = toQuaternion(rawRotation);
				rotIt->second.filter(prime, 0.0f, 0.0f, 0.0f);
			} else if (elapsed >= 0.001f) {
				ControllerPoseFilterState& state = stateIt->second;
				const float rate = std::clamp(1.0f / elapsed, 20.0f, 500.0f);
				glm::vec3 velocityVec(0.0f);
				glm::vec3 angularVelocityVec(0.0f);
				if ((velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
					glm::vec3 candidate(
					    velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z);
					if (IsFinite(candidate))
						velocityVec = candidate;
				}
				if ((velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
					glm::vec3 candidate(
					    velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z);
					if (IsFinite(candidate))
						angularVelocityVec = candidate;
				}

				auto& positionFilter = positionFilters.at(filterKey);
				positionFilter.setFreq(rate);
				state.filteredPosition = positionFilter.filter(rawPosition, velocityVec);

				auto& rotationFilter = rotationFilters.at(filterKey);
				rotationFilter.setFreq(rate);
				Quaternion rawQuaternion = toQuaternion(rawRotation);
				Quaternion filteredQuaternion = rotationFilter.filter(
				    rawQuaternion, angularVelocityVec.x, angularVelocityVec.y, angularVelocityVec.z);
				state.filteredRotation = glm::normalize(toGLMQuat(filteredQuaternion));
				state.lastSample = now;
				state.lastRawPosition = rawPosition;
				state.lastRawRotation = rawRotation;
			}

			// Duplicate pose requests inside one millisecond reuse the prior
			// filtered result instead of advancing a stale shared timestep.
			const ControllerPoseFilterState& output = filterStates.at(filterKey);
			const float outputRotationLengthSquared =
			    glm::dot(output.filteredRotation, output.filteredRotation);
			if (!IsFinite(output.filteredPosition) || !IsFinite(output.filteredRotation) ||
			    !std::isfinite(outputRotationLengthSquared) || outputRotationLengthSquared < 1e-8f) {
				OOVR_LOG_ONCE("Controller smoothing: rejected non-finite filter output; using raw runtime pose");
				ResetControllerPoseFilter(filterKey);
			} else {
				mat = glm::translate(glm::mat4(1.0f), output.filteredPosition) *
				    glm::mat4_cast(glm::normalize(output.filteredRotation));
			}
		}
	}

	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = poseValid;
	pose->mDeviceToAbsoluteTracking = G2S_m34(mat);
	pose->eTrackingResult = pose->bPoseIsValid
	    ? vr::TrackingResult_Running_OK
	    : vr::TrackingResult_Running_OutOfRange;
	pose->vVelocity = X2S_v3f(velocity.linearVelocity);
	pose->vAngularVelocity = X2S_v3f(velocity.angularVelocity);
}
