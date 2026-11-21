//
// Created by ZNix on 24/03/2021.
//

#pragma once

#include "InteractionProfile.h"

// The Touch controllers: the Quest 2's (oculus/touch_controller, with the finger proximity of
// XR_FB_touch_controller_proximity where it's enabled), the Quest Pro's (XR_FB_touch_controller_pro) and the Quest 3's
// Touch Plus (XR_META_touch_controller_plus). All are oculus_touch to OpenVR games.
class OculusTouchInteractionProfile : public InteractionProfile {
public:
	enum class Model { Touch,
		TouchPro,
		TouchPlus };

	explicit OculusTouchInteractionProfile(Model model = Model::Touch, bool proximity = false);

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	LegacyBindings MakeLegacyBindings(const std::string& handPath) const override;

private:
	Model model;
	bool proximity;
};
