#pragma once

#include "InteractionProfile.h"

// Native Frame inputs; registered only when the runtime enables Valve's extension.
class FrameInteractionProfile : public InteractionProfile {
public:
	FrameInteractionProfile();
	const std::string& GetPath() const override;
	std::optional<const char*> GetOpenVRName() const override { return "frame_controller"; }
	std::optional<const char*> GetLeftHandRenderModelName() const override { return "frame_controller_left"; }
	std::optional<const char*> GetRightHandRenderModelName() const override { return "frame_controller_right"; }
protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;
};
