#include "stdafx.h"
#include "FrameInteractionProfile.h"
#include <glm/gtc/matrix_inverse.hpp>

// Paths: https://partner.steamgames.com/doc/steamhardware/steamframe/input
FrameInteractionProfile::FrameInteractionProfile()
{
	for (const auto* side : { "/user/hand/left", "/user/hand/right" }) {
		for (const auto* component : { "bumper", "squeeze", "trigger", "thumbstick" }) {
			for (const auto* state : { "click", "touch" })
				validInputPaths.insert(std::string(side) + "/input/" + component + "/" + state);
		}
		for (const auto* component : { "squeeze/value", "trigger/value", "thumbstick", "thumbstick/x", "thumbstick/y", "grip/pose", "aim/pose" })
			validInputPaths.insert(std::string(side) + "/input/" + component);
		validInputPaths.insert(std::string(side) + "/output/haptic");
	}
	for (const auto* state : { "/click", "/touch" }) {
		for (const auto* button : { "dpad_up", "dpad_down", "dpad_left", "dpad_right", "view" })
			validInputPaths.insert(std::string("/user/hand/left/input/") + button + state);
		for (const auto* button : { "a", "b", "x", "y", "menu" })
			validInputPaths.insert(std::string("/user/hand/right/input/") + button + state);
	}
	// Preserve Touch's common actions when an application has no Frame binding file.
	// Native right X/Y paths are valid and therefore bypass this translation.
	pathTranslationMap = {
		{ "/user/hand/left/input/x", "/user/hand/left/input/dpad_down" },
		{ "/user/hand/left/input/y", "/user/hand/left/input/dpad_up" },
		{ "/user/hand/left/input/menu", "/user/hand/left/input/view" },
		{ "/user/hand/left/input/application_menu", "/user/hand/left/input/view" },
		{ "application_menu", "menu" }, { "joystick", "thumbstick" },
		{ "grip", "squeeze" }, { "pull", "value" }
	};
	// Skyrim selects its Touch controlmap columns using this compatibility identity.
	// Binding-file discovery still uses the native frame_controller name above.
	propertiesMap = {
		{ vr::Prop_ModelNumber_String, { "Steam Frame (Left Controller)", "Steam Frame (Right Controller)" } },
		{ vr::Prop_ControllerType_String, { "oculus_touch" } }
	};
	// Valve's installed Frame render-model openxr_grip transforms, in meters.
	// Do not reuse Quest pose offsets for the native Frame profile.
	float leftOrigin[] = { -0.003117f, -0.004277f, 0.099501f };
	float rightOrigin[] = { 0.003117f, -0.004277f, 0.099501f };
	float rotation[] = { 2.8091f, 0, 0 };
	leftHandGripTransform = glm::affineInverse(ConvertTransform(CustomObject("frame_left", leftOrigin, rotation)));
	rightHandGripTransform = glm::affineInverse(ConvertTransform(CustomObject("frame_right", rightOrigin, rotation)));
}

const std::string& FrameInteractionProfile::GetPath() const
{
	static const std::string path = "/interaction_profiles/valve/frame_controller_valve";
	return path;
}

const InteractionProfile::LegacyBindings* FrameInteractionProfile::GetLegacyBindings(const std::string& handPath) const
{
	static const auto bindings = [] {
		std::array<LegacyBindings, 2> result{};
		for (int hand = 0; hand < 2; ++hand) {
			auto& b = result[hand];
			b.stickX = "input/thumbstick/x"; b.stickY = "input/thumbstick/y";
			b.stickBtn = "input/thumbstick/click"; b.stickBtnTouch = "input/thumbstick/touch";
			b.trigger = "input/trigger/value"; b.triggerClick = "input/trigger/click";
			b.triggerTouch = "input/trigger/touch";
			// Gameplay grab uses analog squeeze, not Frame's end-stop click.
			// OpenXR converts the scalar to a boolean with runtime thresholds,
			// just like our Touch profile. Capacitive finger touch stays separate.
			b.grip = "input/squeeze/value"; b.gripClick = "input/squeeze/value";
			b.gripTouch = "input/squeeze/touch";
			b.btnA = hand == 0 ? "input/dpad_down/click" : "input/a/click";
			b.btnATouch = hand == 0 ? "input/dpad_down/touch" : "input/a/touch";
			b.menu = hand == 0 ? "input/dpad_up/click" : "input/b/click";
			b.menuTouch = hand == 0 ? "input/dpad_up/touch" : "input/b/touch";
			b.frameExtra[0] = hand == 0 ? "input/dpad_left/click" : "input/x/click";
			b.frameExtra[1] = hand == 0 ? "input/dpad_right/click" : "input/y/click";
			b.frameExtra[2] = "input/bumper/click";
			b.frameExtra[3] = hand == 0 ? "input/view/click" : "input/menu/click";
			b.frameExtraTouch[0] = hand == 0 ? "input/dpad_left/touch" : "input/x/touch";
			b.frameExtraTouch[1] = hand == 0 ? "input/dpad_right/touch" : "input/y/touch";
			b.frameExtraTouch[2] = "input/bumper/touch";
			b.frameExtraTouch[3] = hand == 0 ? "input/view/touch" : "input/menu/touch";
			b.haptic = "output/haptic"; b.gripPoseAction = "input/grip/pose"; b.aimPoseAction = "input/aim/pose";
		}
		return result;
	}();
	return &bindings[handPath == "/user/hand/left" ? 0 : 1];
}
