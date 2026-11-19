// Interaction profile of the HTC Vive Cosmos controllers (XR_HTC_vive_focus3_controller_interaction)
#pragma once

#include "InteractionProfile.h"

class ViveFocus3InteractionProfile : public InteractionProfile {
public:
	ViveFocus3InteractionProfile();

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;
};
