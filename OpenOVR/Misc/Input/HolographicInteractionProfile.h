// Interaction profile from the WMR "Holographic" controllers - feature a trackpad and joystick. Samsung's Odyssey
// controllers (XR_EXT_samsung_odyssey_controller) have the same inputs on a profile of their own.
#pragma once

#include "InteractionProfile.h"

class HolographicInteractionProfile : public InteractionProfile {
public:
	explicit HolographicInteractionProfile(bool odyssey = false);

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;

private:
	bool odyssey;
};
