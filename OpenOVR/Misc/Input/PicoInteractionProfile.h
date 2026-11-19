// Interaction profiles of the PICO controllers (XR_BD_controller_interaction): the Neo 3 and PICO 4 ones have the same inputs
#pragma once

#include "InteractionProfile.h"

class PicoInteractionProfile : public InteractionProfile {
public:
	enum class Model { Neo3,
		Pico4 };

	explicit PicoInteractionProfile(Model model);

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;

private:
	Model model;
};
