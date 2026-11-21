//
// Created by ZNix on 24/03/2021.
//

#include "stdafx.h"

#include "OculusInteractionProfile.h"

#include "OculusHandPoses.h"
#include "Reimpl/BaseInput.h"

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/matrix_inverse.hpp>

namespace {
// The Quest 2's as SteamVR names it; the newer controllers' are names of our own, in the same form
std::string ModelNumber(OculusTouchInteractionProfile::Model model, bool left)
{
	const char* name = "Oculus Quest";
	if (model == OculusTouchInteractionProfile::Model::TouchPro)
		name = "Meta Quest Pro";
	else if (model == OculusTouchInteractionProfile::Model::TouchPlus)
		name = "Meta Quest 3";
	return std::string(name) + (left ? " (Left Controller)" : " (Right Controller)");
}
} // namespace

OculusTouchInteractionProfile::OculusTouchInteractionProfile(Model model, bool proximity)
    : model(model)
    , proximity(proximity || model != Model::Touch)
{

	const char* paths[] = {
		"/user/hand/left/input/x/click",
		"/user/hand/left/input/x/touch",
		"/user/hand/left/input/y/click",
		"/user/hand/left/input/y/touch",
		"/user/hand/left/input/menu/click",
		"/user/hand/right/input/a/click",
		"/user/hand/right/input/a/touch",
		"/user/hand/right/input/b/click",
		"/user/hand/right/input/b/touch",
		// Runtimes are not required to support the system button paths, and no OpenVR game can use it anyway.
		//"/user/hand/right/input/system/click",
	};

	const char* perHandPaths[] = {
		"/input/squeeze/value",
		"/input/trigger/value",
		"/input/trigger/touch",
		"/input/thumbstick/x",
		"/input/thumbstick/y",
		"/input/thumbstick",
		"/input/thumbstick/click",
		"/input/thumbstick/touch",
		"/input/thumbrest/touch",
		"/input/grip/pose",
		"/input/aim/pose",
		"/output/haptic",
	};

	for (const char* str : paths) {
		this->validInputPaths.insert(str);
	}

	// Each model's own inputs on both hands: the finger proximity, the trigger's curl and slide, forces and the
	// Pro's stylus tip and extra haptics
	std::vector<const char*> modelPaths;
	switch (model) {
	case Model::Touch:
		if (proximity)
			modelPaths = { "/input/trigger/proximity_fb", "/input/thumb_fb/proximity_fb" };
		break;
	case Model::TouchPro:
		modelPaths = {
			"/input/trigger/proximity_fb",
			"/input/thumb_fb/proximity_fb",
			"/input/trigger/curl_fb",
			"/input/trigger/slide_fb",
			"/input/thumbrest/force",
			"/input/stylus_fb/force",
			"/output/haptic_trigger_fb",
			"/output/haptic_thumb_fb",
		};
		break;
	case Model::TouchPlus:
		modelPaths = {
			"/input/trigger/proximity_meta",
			"/input/thumb_meta/proximity_meta",
			"/input/trigger/curl_meta",
			"/input/trigger/slide_meta",
			"/input/trigger/force",
		};
		break;
	}

	for (const char* str : perHandPaths) {
		this->validInputPaths.insert("/user/hand/left" + std::string(str));
		this->validInputPaths.insert("/user/hand/right" + std::string(str));
	}
	for (const char* str : modelPaths) {
		this->validInputPaths.insert("/user/hand/left" + std::string(str));
		this->validInputPaths.insert("/user/hand/right" + std::string(str));
	}

	// SteamVR's names for the raw inputs the binding evaluator reads (RawComponents)
	this->rawSourceNames = { { "squeeze", "grip" }, { "thumbstick", "joystick" }, { "menu", "application_menu" } };

	// TODO implement the poses through the interaction profile (the raw pose is hard-coded in BaseInput at the moment):
	// pose/raw
	// pose/base
	// pose/handgrip
	// pose/tip

	this->hmdPropertiesMap = {
		{ vr::Prop_TrackingSystemName_String, "oculus" },
		{ vr::Prop_ManufacturerName_String, "Oculus" },
	};

	this->propertiesMap = {
		{ vr::Prop_TrackingSystemName_String, { "oculus" } },
		{ vr::Prop_ModelNumber_String, { ModelNumber(model, true), ModelNumber(model, false) } },
		{ vr::Prop_ControllerType_String, { GetOpenVRName().value() } }
	};

	// Setup the grip-to-steamvr space matrices

	// New Data directly from the openxr-grip space for quest 2 controllers in steamvr
	// SteamVR\resources\rendermodels\oculus_quest2_controller_left
	/*
	    "openxr_grip" : {
	        "component_local":
	        {
	        "origin" : [ -0.007, -0.00182941, 0.1019482 ],
	                   "rotate_xyz" : [ 20.6, 0.0, 0.0 ]
	        }
	    }
	*/

	// Setup the grip-to-steamvr space matrices

	// Made from values in: SteamVR\resources\rendermodels\oculus_quest2_controller_left\oculus_quest2_controller_left.json
	// openxr_grip
	glm::mat4 inverseGripTransformLeft = GetMat4x4FromOriginAndEulerRotations(
	    { 0.007, -0.00182941, 0.1019482 },
	    { 20.6, 0.0, 0.0 });

	// Made from values in: SteamVR\resources\rendermodels\oculus_quest2_controller_right\oculus_quest2_controller_right.json
	// openxr_grip
	glm::mat4 inverseGripTransformRight = GetMat4x4FromOriginAndEulerRotations(
	    { -0.007, -0.00182941, 0.1019482 },
	    { 20.6, 0.0, 0.0 });

	this->leftHandGripTransform = glm::affineInverse(inverseGripTransformLeft);
	this->rightHandGripTransform = glm::affineInverse(inverseGripTransformRight);

	// Set up the component transforms
	this->leftComponentTransforms["base"] = GetMat4x4FromOriginAndEulerRotations(
	    { -0.0034, -0.0034, 0.1491 },
	    { -0.4, 180.0, 0.0 });
	this->rightComponentTransforms["base"] = GetMat4x4FromOriginAndEulerRotations(
	    { 0.0034, -0.0034, 0.1491 },
	    { -0.4, 180.0, 0.0 });
	this->leftComponentTransforms["tip"] = GetMat4x4FromOriginAndEulerRotations(
	    { 0.016694, -0.02522, 0.024687 },
	    { -37.4, 0.0, 0.0 });
	this->rightComponentTransforms["tip"] = GetMat4x4FromOriginAndEulerRotations(
	    { -0.016694, -0.02522, 0.024687 },
	    { -37.4, 0.0, 0.0 });
	
	// Define reference poses
	leftHandPoses[VRSkeletalReferencePose_BindPose] = oculus::leftBindPose;
	leftHandPoses[VRSkeletalReferencePose_OpenHand] = oculus::leftOpenHandPose;
	leftHandPoses[VRSkeletalReferencePose_Fist] = oculus::leftFistPose;
	leftHandPoses[VRSkeletalReferencePose_GripLimit] = oculus::leftGripLimitPose;

	rightHandPoses[VRSkeletalReferencePose_BindPose] = oculus::rightBindPose;
	rightHandPoses[VRSkeletalReferencePose_OpenHand] = oculus::rightOpenHandPose;
	rightHandPoses[VRSkeletalReferencePose_Fist] = oculus::rightFistPose;
	rightHandPoses[VRSkeletalReferencePose_GripLimit] = oculus::rightGripLimitPose;
}

const std::string& OculusTouchInteractionProfile::GetPath() const
{
	static const std::string touch = "/interaction_profiles/oculus/touch_controller";
	static const std::string pro = "/interaction_profiles/facebook/touch_controller_pro";
	static const std::string plus = "/interaction_profiles/meta/touch_controller_plus";
	return model == Model::TouchPro ? pro : model == Model::TouchPlus ? plus : touch;
}

std::optional<const char*> OculusTouchInteractionProfile::GetLeftHandRenderModelName() const
{
	return "oculus_quest2_controller_left";
}

std::optional<const char*> OculusTouchInteractionProfile::GetRightHandRenderModelName() const
{
	return "oculus_quest2_controller_right";
}

std::optional<const char*> OculusTouchInteractionProfile::GetOpenVRName() const
{
	return "oculus_touch";
}

std::optional<vr::EVRSkeletalTrackingLevel> OculusTouchInteractionProfile::GetOpenVRTrackinglevel() const
{
	return vr::VRSkeletalTracking_Estimated;
}

InteractionProfile::LegacyBindings OculusTouchInteractionProfile::MakeLegacyBindings(const std::string& handPath) const
{
	LegacyBindings bindings = {};
	bindings.stickX = "input/thumbstick/x";
	bindings.stickY = "input/thumbstick/y";
	bindings.stickBtn = "input/thumbstick/click";
	bindings.stickBtnTouch = "input/thumbstick/touch";

	bindings.trigger = "input/trigger/value";
	bindings.triggerClick = "input/trigger/value";
	bindings.triggerTouch = "input/trigger/touch";

	bindings.grip = "input/squeeze/value";

	bindings.haptic = "output/haptic";

	bindings.gripPoseAction = "input/grip/pose";
	bindings.aimPoseAction = "input/aim/pose";

	// The fingers near the controller, and the index finger's curl on the trigger, for the estimated skeleton
	if (model == Model::TouchPlus) {
		bindings.triggerProximity = "input/trigger/proximity_meta";
		bindings.thumbProximity = "input/thumb_meta/proximity_meta";
		bindings.triggerCurl = "input/trigger/curl_meta";
	} else if (proximity) {
		bindings.triggerProximity = "input/trigger/proximity_fb";
		bindings.thumbProximity = "input/thumb_fb/proximity_fb";
		if (model == Model::TouchPro)
			bindings.triggerCurl = "input/trigger/curl_fb";
	}

	if (handPath == "/user/hand/left") {
		// Left
		bindings.menu = "input/y/click";
		bindings.menuTouch = "input/y/touch";
		bindings.btnA = "input/x/click";
		bindings.btnATouch = "input/x/touch";

		// Note this refers to what Oculus calls the menu button (and games use to open the pause menu), which
		// is used by SteamVR for it's menu.
		bindings.system = "input/menu/click";
	} else {
		// Right
		bindings.menu = "input/b/click";
		bindings.menuTouch = "input/b/touch";
		bindings.btnA = "input/a/click";
		bindings.btnATouch = "input/a/touch";

		// Ignore Oculus's system button, you're not supposed to do anything with it
	}

	return bindings;
}
