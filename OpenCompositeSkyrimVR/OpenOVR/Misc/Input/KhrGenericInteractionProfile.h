//
// Khronos generic motion-controller fallback profile.
//

#pragma once

#include "OculusInteractionProfile.h"

// XR_KHR_generic_controller deliberately mirrors the common Oculus Touch /
// Index control surface. Inherit the existing Touch pose/model compatibility
// data that Skyrim VR expects, while advertising only the component paths that
// the generic OpenXR profile actually guarantees.
class KhrGenericInteractionProfile : public OculusTouchInteractionProfile {
public:
	KhrGenericInteractionProfile();

	const std::string& GetPath() const override;
	std::optional<const char*> GetOpenVRName() const override;

protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;
};
