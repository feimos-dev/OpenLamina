// The hands as controllers (XR_MSFT_hand_interaction): a pinch (select) and a grasp (squeeze), each a value, and the
// aim and grip poses. OpenVR has no controller type for it: games' bindings for another controller are converted.
#pragma once

#include "InteractionProfile.h"

class MsftHandInteractionProfile : public InteractionProfile {
public:
	MsftHandInteractionProfile();

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;
	std::string BindingConversionType() const override;

protected:
	const LegacyBindings* GetLegacyBindings(const std::string& handPath) const override;
};
