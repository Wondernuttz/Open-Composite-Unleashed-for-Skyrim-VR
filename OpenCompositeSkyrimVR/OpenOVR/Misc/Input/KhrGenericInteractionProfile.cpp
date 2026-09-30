//
// Khronos generic motion-controller fallback profile.
//

#include "stdafx.h"

#include "KhrGenericInteractionProfile.h"

KhrGenericInteractionProfile::KhrGenericInteractionProfile()
{
	// The base Touch constructor gives us Skyrim-compatible controller identity,
	// render-model names, and grip transforms. Replace its physical Touch paths
	// with the exact component set guaranteed by XR_KHR_generic_controller.
	validInputPaths.clear();
	pathTranslationMap.clear();

	const char* const sides[] = {
		"/user/hand/left",
		"/user/hand/right",
	};
	const char* const inputs[] = {
		"input/primary/click",
		"input/secondary/click",
		"input/thumbstick",
		"input/thumbstick/x",
		"input/thumbstick/y",
		"input/thumbstick/click",
		"input/squeeze/value",
		"input/trigger/value",
		"input/grip/pose",
		"input/grip_surface/pose",
		"input/aim/pose",
		"output/haptic",
	};

	for (const char* side : sides) {
		for (const char* input : inputs)
			validInputPaths.insert(std::string(side) + "/" + input);
	}

	// Skyrim VR ships Oculus-style bindings. Khronos defines these same natural
	// equivalences for the generic profile, so translate the binding document as
	// it is imported instead of requiring a PSVR-specific controlmap.
	pathTranslationMap = {
		{ "/input/application_menu", "/input/secondary" },
		{ "/input/x", "/input/primary" },
		{ "/input/a", "/input/primary" },
		{ "/input/y", "/input/secondary" },
		{ "/input/b", "/input/secondary" },
		{ "grip/click", "squeeze/value" },
		{ "trigger/click", "trigger/value" },
		{ "joystick", "thumbstick" },
		{ "grip", "squeeze" },
		{ "pull", "value" },
	};
}

const std::string& KhrGenericInteractionProfile::GetPath() const
{
	static const std::string path = "/interaction_profiles/khr/generic_controller";
	return path;
}

std::optional<const char*> KhrGenericInteractionProfile::GetOpenVRName() const
{
	// Keep this profile in the fallback-binding pass so it imports the best
	// complete application binding (normally Oculus Touch). The inherited
	// controller properties still identify it to Skyrim as Oculus-compatible.
	return std::nullopt;
}

const InteractionProfile::LegacyBindings* KhrGenericInteractionProfile::GetLegacyBindings(
	const std::string& handPath) const
{
	static LegacyBindings allBindings[2] = { {}, {} };
	const int hand = handPath == "/user/hand/left" ? 0 : 1;
	LegacyBindings& bindings = allBindings[hand];

	if (!bindings.menu) {
		bindings.stickX = "input/thumbstick/x";
		bindings.stickY = "input/thumbstick/y";
		bindings.stickBtn = "input/thumbstick/click";

		bindings.trigger = "input/trigger/value";
		bindings.triggerClick = "input/trigger/value";

		bindings.grip = "input/squeeze/value";
		bindings.gripClick = "input/squeeze/value";

		bindings.btnA = "input/primary/click";
		bindings.menu = "input/secondary/click";

		bindings.haptic = "output/haptic";
		bindings.gripPoseAction = "input/grip/pose";
		bindings.aimPoseAction = "input/aim/pose";
	}

	return &bindings;
}
