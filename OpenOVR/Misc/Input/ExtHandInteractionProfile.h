// The hands as controllers (XR_EXT_hand_interaction): a pinch and a grasp, each a value, and the aim, grip, pinch and
// poke poses. Games see the hands as another controller (an Index controller unless the handController setting says
// otherwise): its controller type, render models and properties, and its binding files, the pinch being the trigger,
// the grasp the grip and the aim pose the pointer. Also the profile of a hand with no controller while hand tracking
// runs, which has no inputs then.
#pragma once

#include "InteractionProfile.h"

#include <memory>

class ExtHandInteractionProfile : public InteractionProfile {
public:
	/** emulated: the controller the hands are shown as. bindable: XR_EXT_hand_interaction is enabled, so the profile
	 * takes suggestions. */
	ExtHandInteractionProfile(std::unique_ptr<InteractionProfile> emulated, bool bindable);

	const std::string& GetPath() const override;
	std::optional<const char*> GetLeftHandRenderModelName() const override;
	std::optional<const char*> GetRightHandRenderModelName() const override;
	std::optional<const char*> GetOpenVRName() const override;
	std::optional<vr::EVRSkeletalTrackingLevel> GetOpenVRTrackinglevel() const override;
	/** False while libmonado lists a controller in a hand role; decided once per session */
	bool CanHaveBindings() const override;
	/** A new session decides again */
	static void ForgetDecision();

	static bool ControllerInHands(const std::optional<std::string>& left, const std::optional<std::string>& right);

protected:
	LegacyBindings MakeLegacyBindings(const std::string& handPath) const override;

private:
	std::unique_ptr<InteractionProfile> emulated;
	bool bindable;
};
