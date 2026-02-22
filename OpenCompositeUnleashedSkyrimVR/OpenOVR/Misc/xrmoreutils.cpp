//
// Created by ZNix on 15/03/2021.
//

#include "stdafx.h"

#include "Misc/Config.h"
#include "OneEuroFilterPosition.cpp"
#include "OneEuroFilterRotation.cpp"
#include "xrmoreutils.h"
#include <chrono>
#include <convert.h>
#include <map>

bool isInitialized = false;

static std::map<int, OneEuroFilterRotation> rotationFilters;
static std::map<int, OneEuroFilterPosition> posFilters;

static std::map<int, bool> initialized;

glm::vec3 toEulerAngles(const glm::quat& q)
{
	glm::vec3 angles;

	// Roll (x-axis rotation)
	double sinr_cosp = +2.0 * (q.w * q.x + q.y * q.z);
	double cosr_cosp = +1.0 - 2.0 * (q.x * q.x + q.y * q.y);
	angles.x = atan2(sinr_cosp, cosr_cosp);

	// Pitch (y-axis rotation)
	double sinp = +2.0 * (q.w * q.y - q.z * q.x);
	if (fabs(sinp) >= 1)
		angles.y = copysign(M_PI / 2, sinp); // Use 90 degrees if out of range
	else
		angles.y = asin(sinp);

	// Yaw (z-axis rotation)
	double siny_cosp = +2.0 * (q.w * q.z + q.x * q.y);
	double cosy_cosp = +1.0 - 2.0 * (q.y * q.y + q.z * q.z);
	angles.z = atan2(siny_cosp, cosy_cosp);

	return angles;
}

// Converts from glm::quat to Quaternion
Quaternion toQuaternion(const glm::quat& q)
{
	return Quaternion(q.x, q.y, q.z, q.w);
}

// Converts from Quaternion to glm::quat
glm::quat toGLMQuat(const Quaternion& q)
{
	return glm::quat(q.w, q.x, q.y, q.z);
}

static float dt;

void xr_utils::PoseFromSpace(vr::TrackedDevicePose_t* pose, XrSpace space, vr::ETrackingUniverseOrigin origin, std::optional<glm::mat4> extraTransform, int device)
{
	auto baseSpace = xr_space_from_tracking_origin(origin);

	XrSpaceVelocity velocity{ XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation info{ XR_TYPE_SPACE_LOCATION, &velocity, 0, {} };

	OOVR_FAILED_XR_SOFT_ABORT(xrLocateSpace(space, baseSpace, xr_gbl->GetBestTime(), &info));

	glm::mat4 mat = X2G_om34_pose(info.pose);

	if (extraTransform) {
		mat = mat * extraTransform.value();

		if (oovr_global_configuration.EnableControllerSmoothing()) {
			// Initialization and time computations
			static std::map<int, long long> previousTimes;
			long long currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

			// OOVR_LOGF("currentTime %d", currentTime);

			if (previousTimes[device] && currentTime - previousTimes[device] > 3) {
				dt = (currentTime - previousTimes[device]) / 1000.0;
			}

			previousTimes[device] = currentTime;

			if (dt) {
				float rate = 1.0 / dt;
				// OOVR_LOGF("dt: %f rate: %f", dt, rate);

				glm::vec3 position = glm::vec3(mat[3]);
				glm::vec3 velocityVec(velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z);
				glm::vec3 angularVelocityVec(velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z);

				// Get the current rotation as a quaternion
				glm::quat currentRotation = glm::quat_cast(mat);

				if (posFilters.find(device) == posFilters.end()) {
					posFilters[device] = OneEuroFilterPosition(rate, oovr_global_configuration.PosSmoothMinCutoff(), oovr_global_configuration.PosSmoothBeta(), 1);
				} else {
					posFilters[device].setFreq(rate);
				}

				position = posFilters[device].filter(position, velocityVec);

				if (rotationFilters.find(device) == rotationFilters.end()) {
					rotationFilters[device] = OneEuroFilterRotation(rate, oovr_global_configuration.RotSmoothMinCutoff(), oovr_global_configuration.RotSmoothBeta(), 1);
				} else {
					rotationFilters[device].setFreq(rate);
				}

				// Convert glm::quat to Quaternion
				Quaternion tempQuat = toQuaternion(currentRotation);

				// Apply the filter on the converted Quaternion
				Quaternion filteredQuat = rotationFilters[device].filter(tempQuat, angularVelocityVec.x, angularVelocityVec.y, angularVelocityVec.z);

				// Convert the filtered Quaternion back to glm::quat
				currentRotation = toGLMQuat(filteredQuat);

				// Normalize the quaternion to ensure it's a valid rotation
				currentRotation = glm::normalize(currentRotation);

				glm::mat4 rotMat = glm::mat4_cast(currentRotation);
				glm::mat4 transMat = glm::translate(glm::mat4(1.0f), position);

				mat = transMat * rotMat; //
			}
		}
	}

	pose->bDeviceIsConnected = true;
	pose->bPoseIsValid = (info.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
	pose->mDeviceToAbsoluteTracking = G2S_m34(mat);
	pose->eTrackingResult = pose->bPoseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
	pose->vVelocity = X2S_v3f(velocity.linearVelocity); // No offsetting transform - this is in world-space
	pose->vAngularVelocity = X2S_v3f(velocity.angularVelocity); // TODO find out if this needs a transform
}
