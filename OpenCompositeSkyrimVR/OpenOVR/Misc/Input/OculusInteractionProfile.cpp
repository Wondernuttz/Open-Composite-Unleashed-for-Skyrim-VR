//
// Created by ZNix on 24/03/2021.
//

#include "stdafx.h"

#include "OculusInteractionProfile.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

OculusTouchInteractionProfile::OculusTouchInteractionProfile()
{

	const char* paths[] = {
		"/user/hand/left/input/x/click",
		"/user/hand/left/input/x/touch",
		"/user/hand/left/input/y/click",
		"/user/hand/left/input/y/touch",
		"/user/hand/left/input/menu/click",
		"/user/hand/right/input/a/click",
		"/user/hand/right/input/a/touch",
		"/user/hand/right/input/b/click",
		"/user/hand/right/input/b/touch",
		// Runtimes are not required to support the system button paths, and no OpenVR game can use it anyway.
		//"/user/hand/right/input/system/click",
	};

	const char* perHandPaths[] = {
		"input/squeeze/value",
		"input/trigger/value",
		"input/trigger/touch",
		"input/thumbstick/x",
		"input/thumbstick/y",
		"input/thumbstick/click",
		"input/thumbstick/touch",
		"input/thumbstick",
		"input/thumbrest/touch",
		"input/grip/pose",
		"input/aim/pose",
		"output/haptic",
	};

	for (const char* str : paths) {
		validInputPaths.insert(str);
	}

	for (const char* str : perHandPaths) {
		validInputPaths.insert("/user/hand/left/" + std::string(str));
		validInputPaths.insert("/user/hand/right/" + std::string(str));
	}

	pathTranslationMap = {
		{ "grip", "squeeze" },
		{ "joystick", "thumbstick" },
		{ "pull", "value" },
		{ "grip/click", "squeeze/value" },
		{ "trigger/click", "trigger/value" },
		{ "application_menu", "menu" }
	};
	// TODO implement the poses through the interaction profile (the raw pose is hard-coded in BaseInput at the moment):
	// pose/raw
	// pose/base
	// pose/handgrip
	// pose/tip

	hmdPropertiesMap = {
		{ vr::Prop_ManufacturerName_String, "Oculus" },
	};

	propertiesMap = {
		{ vr::Prop_ModelNumber_String, { "Oculus Quest2 (Left Controller)", "Oculus Quest2 (Right Controller)" } },
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};

	// Setup the grip-to-steamvr space matrices

	// New Data directly from the openxr-grip space for quest 2 controllers in steamvr
	// SteamVR\resources\rendermodels\oculus_quest2_controller_left
	/*
	    "openxr_grip" : {
	        "component_local":
	        {
	        "origin" : [ -0.007, -0.00182941, 0.1019482 ],
	                   "rotate_xyz" : [ 20.6, 0.0, 0.0 ]
	        }
	    }
	*/

	// Setup the grip-to-steamvr space matrices

	float originLeft[3] = { 0.0, 0.003, 0.097 };
	float rotationLeft[3] = { 5.037, 0.0, 0.0 };
	CustomObject ctrlTransformLeft("handgrip_left", originLeft, rotationLeft);

	float originRight[3] = { 0.0, 0.003, 0.097 };
	float rotationRight[3] = { 5.037, 0.0, 0.0 };
	CustomObject ctrlTransformRight("handgrip_right", originRight, rotationRight);

	float originLeft2[3] = { 0.0, 0.003, 0.097 };
	float rotationLeft2[3] = { 0.037, 0.0, 0.0 };
	CustomObject ctrlTransformLeft2("handgrip_left", originLeft2, rotationLeft2);

	float originRight2[3] = { 0.0, 0.003, 0.097 };
	float rotationRight2[3] = { 0.037, 0.0, 0.0 };
	CustomObject ctrlTransformRight2("handgrip_right", originRight2, rotationRight2);

	float originBaseLeft[3] = { -0.00554, -0.00735, 0.139 };
	float rotationBaseLeft[3] = { -0.4, -180.0, 0.0 };
	CustomObject baseTransformLeft("base_left", originBaseLeft, rotationBaseLeft);

	float originBaseRight[3] = { 0.00554, -0.00735, 0.139 };
	float rotationBaseRight[3] = { -0.4, -180.0, 0.0 };
	CustomObject baseTransformRight("base_right", originBaseRight, rotationBaseRight);

	float originBaseLeftNoRot[3] = { -0.00554, 0.00635, 0.000 };
	float rotationBaseLeftNoRot[3] = { -20, 0.0, 0.0 };
	CustomObject baseTransformLeftNoRot("base_leftnorot", originBaseLeftNoRot, rotationBaseLeftNoRot);

	float originBaseRightNoRot[3] = { 0.00554, 0.00635, 0.000 };
	float rotationBaseRightNoRot[3] = { -20, 0.0, 0.0 };
	CustomObject baseTransformRightNoRot("base_rightnorot", originBaseRightNoRot, rotationBaseRightNoRot);

	float originBodyLeft[3] = { 0.0, 0.003, 0.097 };
	float rotationBodyLeft[3] = { 5.037, 0.0, 0.0 };
	CustomObject bodyTransformLeft("body_left", originBodyLeft, rotationBodyLeft);

	float originBodyRight[3] = { 0.0, 0.003, 0.097 };
	float rotationBodyRight[3] = { 5.037, 0.0, 0.0 };
	CustomObject bodyTransformRight("body_right", originBodyRight, rotationBodyRight);

	float originTipLeft[3] = { 0.00629, -0.02522, 0.03469 };
	float rotationTipLeft[3] = { -39.4, 0.0, 0.0 };
	CustomObject tipTransformLeft("tip_left", originTipLeft, rotationTipLeft);

	float originTipRight[3] = { -0.00629, -0.02522, 0.03469 };
	float rotationTipRight[3] = { -39.4, 0.0, 0.0 };
	CustomObject tipTransformRight("tip_right", originTipRight, rotationTipRight);

	float originEmptyLeft[3] = { 0.0, 0.0, 0.0 };
	float rotationEmptyLeft[3] = { 0.0, 0.0, 0.0 };
	CustomObject emptyTransformLeft("body_left", originEmptyLeft, rotationEmptyLeft);

	float originEmptyRight[3] = { 0.0, 0.0, 0.0 };
	float rotationEmptyRight[3] = { 0.0, 0.0, 0.0 };
	CustomObject emptyTransformRight("body_right", originEmptyRight, rotationEmptyRight);

	// leftHandGripTransform = glm::affineInverse(convertTransform(baseTransformLeftNoRot) * convertTransform(ctrlTransformLeft));
	// rightHandGripTransform = glm::affineInverse(convertTransform(baseTransformRightNoRot) * convertTransform(ctrlTransformRight));

	leftHandGripTransform = glm::affineInverse(ConvertTransform(ctrlTransformLeft2));
	rightHandGripTransform = glm::affineInverse(ConvertTransform(ctrlTransformRight2));

	leftComponentTransforms["body"] = glm::affineInverse(ConvertTransform(bodyTransformLeft));
	rightComponentTransforms["body"] = glm::affineInverse(ConvertTransform(bodyTransformRight));
	leftComponentTransforms["base"] = glm::affineInverse(ConvertTransform(baseTransformLeft));
	rightComponentTransforms["base"] = glm::affineInverse(ConvertTransform(baseTransformRight));
	leftComponentTransforms["tip"] = glm::affineInverse(ConvertTransform(tipTransformLeft));
	rightComponentTransforms["tip"] = glm::affineInverse(ConvertTransform(tipTransformRight));
}

const std::string& OculusTouchInteractionProfile::GetPath() const
{
	static std::string path = "/interaction_profiles/oculus/touch_controller";
	return path;
}

std::optional<const char*> OculusTouchInteractionProfile::GetLeftHandRenderModelName() const
{
	return "oculus_quest2_controller_left";
}

std::optional<const char*> OculusTouchInteractionProfile::GetRightHandRenderModelName() const
{
	return "oculus_quest2_controller_right";
}

std::optional<const char*> OculusTouchInteractionProfile::GetOpenVRName() const
{
	return "oculus_touch";
}

const InteractionProfile::LegacyBindings* OculusTouchInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
	static LegacyBindings allBindings[2] = { {}, {} };
	int hand = handPath == "/user/hand/left" ? vr::Eye_Left : vr::Eye_Right;
	LegacyBindings& bindings = allBindings[hand];

	// First-time initialisation
	if (!bindings.menu) {
		bindings = {};
		bindings.stickX = "input/thumbstick/x";
		bindings.stickY = "input/thumbstick/y";
		bindings.stickBtn = "input/thumbstick/click";
		bindings.stickBtnTouch = "input/thumbstick/touch";

		bindings.trigger = "input/trigger/value";
		bindings.triggerClick = "input/trigger/value";
		bindings.triggerTouch = "input/trigger/touch";

		bindings.grip = "input/squeeze/value";

		bindings.haptic = "output/haptic";

		bindings.gripPoseAction = "input/grip/pose";
		bindings.aimPoseAction = "input/aim/pose";

		if (handPath == "/user/hand/left") {
			// Left
			bindings.menu = "input/y/click";
			bindings.menuTouch = "input/y/touch";
			bindings.btnA = "input/x/click";
			bindings.btnATouch = "input/x/touch";

			// Note this refers to what Oculus calls the menu button (and games use to open the pause menu), which
			// is used by SteamVR for it's menu.
			bindings.system = "input/menu/click";
		} else {
			// Right
			bindings.menu = "input/b/click";
			bindings.menuTouch = "input/b/touch";
			bindings.btnA = "input/a/click";
			bindings.btnATouch = "input/a/touch";

			// Ignore Oculus's system button, you're not supposed to do anything with it
		}
	}

	return &bindings;
}
