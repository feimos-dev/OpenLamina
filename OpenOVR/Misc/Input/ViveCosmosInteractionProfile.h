// Interaction profile of the HTC Vive Cosmos controllers (XR_HTC_vive_cosmos_controller_interaction)
#pragma once

#include "InteractionProfile.h"

class ViveCosmosInteractionProfile : public InteractionProfile {
public:
	ViveCosmosInteractionProfile();

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	LegacyBindings MakeLegacyBindings(const std::string& handPath) const override;
};
