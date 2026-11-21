// Interaction profile from the WMR "Holographic" controllers - feature a trackpad and joystick. Samsung's Odyssey
// controllers (XR_EXT_samsung_odyssey_controller) have the same inputs on a profile of their own.
#pragma once

#include "InteractionProfile.h"

class HolographicInteractionProfile : public InteractionProfile {
public:
	enum class Model { MotionController,
		Odyssey };

	explicit HolographicInteractionProfile(Model model = Model::MotionController);

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	LegacyBindings MakeLegacyBindings(const std::string& handPath) const override;

private:
	Model model;
};
