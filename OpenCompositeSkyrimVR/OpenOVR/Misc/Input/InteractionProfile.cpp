//
// Created by ZNix on 27/02/2021.
//

#include "stdafx.h"

#include <utility>

#include "HolographicInteractionProfile.h"
#include "IndexControllerInteractionProfile.h"
#include "InteractionProfile.h"
#include "KhrSimpleInteractionProfile.h"
#include "OculusInteractionProfile.h"
#include "ReverbG2InteractionProfile.h"
#include "ViveInteractionProfile.h"

#include "Reimpl/BaseInput.h"
#include "generated/static_bases.gen.h"
#include <Misc/Config.h>

std::string InteractionProfile::TranslateAction(const std::string& inputPath) const
{
	if (!pathTranslationMap.empty() && !IsInputPathValid(inputPath)) {
		std::string ret = inputPath;
		for (auto& [key, val] : pathTranslationMap) {
			size_t loc = ret.find(key);
			if (loc != std::string::npos) {
				// translate action!
				ret = ret.substr(0, loc) + val + ret.substr(loc + key.size());
			}
		}
		OOVR_LOGF("Translated path %s to %s for profile %s", inputPath.c_str(), ret.c_str(), GetPath().c_str());
		return ret;
	}
	// either this path is already valid or it's invalid and not translatable
	return inputPath;
}

const std::unordered_set<std::string>& InteractionProfile::GetValidInputPaths() const
{
	return validInputPaths;
}

bool InteractionProfile::IsInputPathValid(const std::string& inputPath) const
{
	return validInputPaths.find(inputPath) != validInputPaths.end();
}

void InteractionProfile::AddLegacyBindings(const LegacyControllerActions& ctrl, std::vector<XrActionSuggestedBinding>& bindings) const
{
	auto create = [&](XrAction action, const char* path) {
		// Not supported on this controller?
		if (path == nullptr)
			return;

		// Bind the action to a suggested binding
		std::string realPath = ctrl.handPath + "/" + path;
		if (!IsInputPathValid(realPath)) {
			// No need for a soft abort, will only be called if an input profile gets it's paths wrong
			OOVR_ABORTF("Found legacy input path %s, not supported by profile", realPath.c_str());
		}

		// The action must have been defined, if not we'll get a harder-to-debug error from OpenXR later.
		OOVR_FALSE_ABORT(action != XR_NULL_HANDLE);

		XrActionSuggestedBinding binding = {};
		binding.action = action;

		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, realPath.c_str(), &binding.binding));

		bindings.push_back(binding);
	};

	const LegacyBindings* paths = GetLegacyBindings(ctrl.handPath);

	create(ctrl.system, paths->system);
	create(ctrl.menu, paths->menu);
	create(ctrl.menuTouch, paths->menuTouch);
	create(ctrl.btnA, paths->btnA);
	create(ctrl.btnATouch, paths->btnATouch);
	create(ctrl.stickX, paths->stickX);
	create(ctrl.stickY, paths->stickY);
	create(ctrl.trackPadX, paths->trackPadX);
	create(ctrl.trackPadY, paths->trackPadY);
	create(ctrl.trackPadClick, paths->trackPadClick);
	create(ctrl.trackPadTouch, paths->trackPadTouch);
	create(ctrl.stickBtn, paths->stickBtn);
	create(ctrl.stickBtnTouch, paths->stickBtnTouch);
	create(ctrl.trigger, paths->trigger);
	create(ctrl.triggerTouch, paths->triggerTouch);
	create(ctrl.triggerClick, paths->triggerClick);
	create(ctrl.grip, paths->grip);
	create(ctrl.gripClick, paths->gripClick != NULL ? paths->gripClick : paths->grip);
	create(ctrl.haptic, paths->haptic);
	create(ctrl.gripPoseAction, paths->gripPoseAction);
	create(ctrl.aimPoseAction, paths->aimPoseAction);
}

const InteractionProfile::ProfileList& InteractionProfile::GetProfileList()
{
	static std::vector<std::unique_ptr<InteractionProfile>> profiles;
	if (profiles.empty()) {
		if (xr_ext->G2Controller_Available())
			profiles.emplace_back(std::make_unique<ReverbG2InteractionProfile>());

		profiles.emplace_back(std::make_unique<HolographicInteractionProfile>());
		profiles.emplace_back(std::make_unique<IndexControllerInteractionProfile>());
		profiles.emplace_back(std::make_unique<ViveWandInteractionProfile>());
		profiles.emplace_back(std::make_unique<OculusTouchInteractionProfile>());
		profiles.emplace_back(std::make_unique<KhrSimpleInteractionProfile>());
	}
	return profiles;
}

InteractionProfile* InteractionProfile::GetProfileByPath(const string& name)
{
	static std::map<std::string, InteractionProfile*> byPath;
	if (byPath.empty()) {
		for (const std::unique_ptr<InteractionProfile>& profile : GetProfileList()) {
			byPath[profile->GetPath()] = profile.get();
		}
	}
	if (!byPath.contains(name))
		OOVR_ABORTF("Could not find interaction profile '%s'", name.c_str());
	return byPath.at(name);
}

glm::mat4 InteractionProfile::ConvertTransform(CustomObject ctrlTransforms) const
{
	// Convert the rotation values from degrees to radians
	float rx = ctrlTransforms.get_array2()[0] * (3.14159265359 / 180);
	float ry = ctrlTransforms.get_array2()[1] * (3.14159265359 / 180);
	float rz = ctrlTransforms.get_array2()[2] * (3.14159265359 / 180);

	// Calculate the sin and cosine values for the rotation angles
	float sx = sin(rx);
	float cx = cos(rx);
	float sy = sin(ry);
	float cy = cos(ry);
	float sz = sin(rz);
	float cz = cos(rz);

	// Define the transformation matrix
	return {
		{ cy * cz, -cy * sz, sy, 0 },
		{ cz * sx * sy + cx * sz, cx * cz - sx * sy * sz, -cy * sx, 0 },
		{ -cx * cz * sy + sx * sz, cz * sx + cx * sy * sz, cx * cy, 0 },
		{ ctrlTransforms.get_array1()[0], ctrlTransforms.get_array1()[1], ctrlTransforms.get_array1()[2], 1 }
	};
}

glm::mat4 InteractionProfile::CreateRotationMatrix(float xDegrees, float yDegrees, float zDegrees) const
{
	glm::mat4 xRot = glm::rotate(glm::mat4(1.0f), glm::radians(xDegrees), glm::vec3(1.0f, 0.0f, 0.0f));
	glm::mat4 yRot = glm::rotate(glm::mat4(1.0f), glm::radians(yDegrees), glm::vec3(0.0f, 1.0f, 0.0f));
	glm::mat4 zRot = glm::rotate(glm::mat4(1.0f), glm::radians(zDegrees), glm::vec3(0.0f, 0.0f, 1.0f));
	return xRot * yRot * zRot;
}

glm::mat4 InteractionProfile::GetGripToSteamVRTransform(ITrackedDevice::HandType hand) const
{
	bool adjustLeftRotation = oovr_global_configuration.AdjustLeftRotation();
	bool adjustRightRotation = oovr_global_configuration.AdjustRightRotation();
	bool adjustLeftPosition = oovr_global_configuration.AdjustLeftPosition();
	bool adjustRightPosition = oovr_global_configuration.AdjustRightPosition();
	bool adjustTilt = oovr_global_configuration.AdjustTilt();
	float tiltDegrees = oovr_global_configuration.Tilt();

	float positionLeft[3] = { 0.0, 0.0, 0.0 };
	float rotationLeft[3] = { 0.0, 0.0, 0.0 };
	if (adjustLeftPosition) {
		positionLeft[0] = oovr_global_configuration.LeftXPosition();
		positionLeft[1] = oovr_global_configuration.LeftYPosition();
		positionLeft[2] = oovr_global_configuration.LeftZPosition();
	}
	if (adjustLeftRotation) {
		rotationLeft[0] = oovr_global_configuration.LeftXRotation();
		rotationLeft[1] = oovr_global_configuration.LeftYRotation();
		rotationLeft[2] = oovr_global_configuration.LeftZRotation();
	}
	CustomObject ctrlTransformLeft("adjust_left", positionLeft, rotationLeft);

	float positionRight[3] = { 0.0, 0.0, 0.0 };
	float rotationRight[3] = { 0.0, 0.0, 0.0 };
	if (adjustRightPosition) {
		positionRight[0] = oovr_global_configuration.RightXPosition();
		positionRight[1] = oovr_global_configuration.RightYPosition();
		positionRight[2] = oovr_global_configuration.RightZPosition();
	}
	if (adjustRightRotation) {
		rotationRight[0] = oovr_global_configuration.RightXRotation();
		rotationRight[1] = oovr_global_configuration.RightYRotation();
		rotationRight[2] = oovr_global_configuration.RightZRotation();
	}
	CustomObject ctrlTransformRight("adjust_right", positionRight, rotationRight);

	glm::mat4 rotationMatrix = CreateRotationMatrix(tiltDegrees, 0, 0);

	if (hand == ITrackedDevice::HandType::HAND_LEFT) {
		if (adjustLeftRotation || adjustLeftPosition) {
			return leftHandGripTransform * ConvertTransform(ctrlTransformLeft);
		} else if (adjustTilt) {
			return leftHandGripTransform * rotationMatrix;
		}
		return leftHandGripTransform;
	} else if (hand == ITrackedDevice::HandType::HAND_RIGHT) {
		if (adjustRightRotation || adjustRightPosition) {
			return rightHandGripTransform * ConvertTransform(ctrlTransformRight);
		} else if (adjustTilt) {
			return rightHandGripTransform * rotationMatrix;
		}
		return rightHandGripTransform;
	}

	return glm::identity<glm::mat4>();
}

std::optional<glm::mat4> InteractionProfile::GetComponentTransform(ITrackedDevice::HandType hand, const std::string& name) const
{
	std::unordered_map<std::string, glm::mat4> transforms = hand == ITrackedDevice::HAND_RIGHT ? rightComponentTransforms : leftComponentTransforms;
	const auto iter = transforms.find(name);
	if (iter == transforms.end())
		return {};
	else
		return iter->second;
}
