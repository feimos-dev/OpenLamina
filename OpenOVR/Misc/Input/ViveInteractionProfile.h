#pragma once

#include "InteractionProfile.h"

class ViveWandInteractionProfile : public InteractionProfile {
public:
	ViveWandInteractionProfile();

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;

protected:
	LegacyBindings MakeLegacyBindings(const std::string& handPath) const override;
};
