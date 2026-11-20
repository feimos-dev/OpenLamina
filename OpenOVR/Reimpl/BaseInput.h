#pragma once

// Make this usable from DrvOpenXR to refresh the inputs after swapping sessions
// FIXME don't do that, it's ugly and slows down the build when modifying headers

#include "Drivers/Backend.h"
#include "Misc/Input/ViveTrackerRoles.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "Misc/Input/Bindings/BindingInfo.h"
#include "Misc/Input/Bindings/Evaluator.h"
#include "Misc/Input/InputData.h"
#include "Misc/Input/InteractionProfile.h"
#include "Misc/Input/LegacyControllerActions.h"
#include "generated/interfaces/vrannotation.h"

typedef std::array<vr::VRBoneTransform_t, 31> BoneArray;
typedef vr::EVRSkeletalTrackingLevel OOVR_EVRSkeletalTrackingLevel;

enum OOVR_EVRSkeletalReferencePose {
	VRSkeletalReferencePose_BindPose = 0,
	VRSkeletalReferencePose_OpenHand,
	VRSkeletalReferencePose_Fist,
	VRSkeletalReferencePose_GripLimit
};

enum OOVR_EVRFinger {
	VRFinger_Thumb = 0,
	VRFinger_Index,
	VRFinger_Middle,
	VRFinger_Ring,
	VRFinger_Pinky,
	VRFinger_Count
};

enum OOVR_EVRFingerSplay {
	VRFingerSplay_Thumb_Index = 0,
	VRFingerSplay_Index_Middle,
	VRFingerSplay_Middle_Ring,
	VRFingerSplay_Ring_Pinky,
	VRFingerSplay_Count
};

enum OOVR_EVRSummaryType {
	// The skeletal summary data will match the animated bone transforms for the action.
	VRSummaryType_FromAnimation = 0,

	// The skeletal summary data will include unprocessed data directly from the device when available.
	// This data is generally less latent than the data that is computed from the animations.
	VRSummaryType_FromDevice = 1,
};

enum OOVR_EVRInputStringBits {
	VRInputString_Hand = 0x01,
	VRInputString_ControllerType = 0x02,
	VRInputString_InputSource = 0x04,

	VRInputString_All = 0xFFFFFFFF
};

enum OOVR_EVRSkeletalTransformSpace {
	VRSkeletalTransformSpace_Model = 0,
	VRSkeletalTransformSpace_Parent = 1,
};

enum OOVR_EVRInputFilterCancelType {
	VRInputFilterCancel_Timers = 0,
	VRInputFilterCancel_Momentum = 1,
};

// From https://github.com/ValveSoftware/openvr/wiki/Hand-Skeleton
// Used as indexes into the skeleton output data
enum HandSkeletonBone {
	eBone_Root = 0,
	eBone_Wrist,
	eBone_Thumb0,
	eBone_Thumb1,
	eBone_Thumb2,
	eBone_Thumb3,
	eBone_IndexFinger0,
	eBone_IndexFinger1,
	eBone_IndexFinger2,
	eBone_IndexFinger3,
	eBone_IndexFinger4,
	eBone_MiddleFinger0,
	eBone_MiddleFinger1,
	eBone_MiddleFinger2,
	eBone_MiddleFinger3,
	eBone_MiddleFinger4,
	eBone_RingFinger0,
	eBone_RingFinger1,
	eBone_RingFinger2,
	eBone_RingFinger3,
	eBone_RingFinger4,
	eBone_PinkyFinger0,
	eBone_PinkyFinger1,
	eBone_PinkyFinger2,
	eBone_PinkyFinger3,
	eBone_PinkyFinger4,
	eBone_Aux_Thumb,
	eBone_Aux_IndexFinger,
	eBone_Aux_MiddleFinger,
	eBone_Aux_RingFinger,
	eBone_Aux_PinkyFinger,
	eBone_Count
};

// IVRInput_003 (SDK 1.0.15) declares its own handles and skeleton data, the same as today's
typedef vr::VRActionHandle_t OOVR_VRActionHandle_t;
typedef vr::VRActionSetHandle_t OOVR_VRActionSetHandle_t;
typedef vr::VRInputValueHandle_t OOVR_VRInputValueHandle_t;
struct OOVR_InputSkeletalActionData_t;
typedef OOVR_InputSkeletalActionData_t OOVR_InputSkeletonActionData_t;

struct OOVR_InputSkeletalActionData_t {
	/** Whether or not this action is currently available to be bound in the active action set */
	bool bActive;

	/** The origin that caused this action's current state */
	vr::VRInputValueHandle_t activeOrigin;
};

struct OOVR_InputOriginInfo_t {
	vr::VRInputValueHandle_t devicePath;
	vr::TrackedDeviceIndex_t trackedDeviceIndex;
	char rchRenderModelComponentName[128];
};

struct OOVR_VRActiveActionSet_t {
	/** This is the handle of the action set to activate for this frame. */
	vr::VRActionSetHandle_t ulActionSet;

	/** This is the handle of a device path that this action set should be active for. To
	 * activate for all devices, set this to k_ulInvalidInputValueHandle. */
	vr::VRInputValueHandle_t ulRestrictedToDevice;

	/** The action set to activate for all devices other than ulRestrictedDevice. If
	 * ulRestrictedToDevice is set to k_ulInvalidInputValueHandle, this parameter is
	 * ignored. */
	vr::VRActionSetHandle_t ulSecondaryActionSet;

	// This field is ignored
	uint32_t unPadding;

	/** The priority of this action set relative to other action sets. Any inputs
	 * bound to a source (e.g. trackpad, joystick, trigger) will disable bindings in
	 * other active action sets with a smaller priority. */
	int32_t nPriority;
};

/** Contains summary information about the current skeletal pose */
struct OOVR_VRSkeletalSummaryData_t {
	/** The amount that each finger is 'curled' inwards towards the palm.  In the case of the thumb,
	 * this represents how much the thumb is wrapped around the fist.
	 * 0 means straight, 1 means fully curled */
	float flFingerCurl[VRFinger_Count];

	/** The amount that each pair of adjacent fingers are separated.
	 * 0 means the digits are touching, 1 means they are fully separated.
	 */
	float flFingerSplay[VRFingerSplay_Count];
};

struct OOVR_InputBindingInfo_t {
	char rchDevicePathName[128]; // The device path of the bound source, eg '/user/hand/left'
	char rchInputPathName[128]; // The input path of the bound source, eg '/input/trigger'
	char rchModeName[128]; // The binding's mode in string form, eg 'button'
	char rchSlotName[128]; // The binding's slot, eg 'click'

	/**
	 * The type of the physical input that the user actuates. For example a button bound to the thumbstick click
	 * returns 'joystick' and the trigger mapped as a digital button returns 'trigger'.
	 *
	 * The known values are:
	 * - joystick (the thumbstick)
	 * - trigger (the main trigger and the grip)
	 * - button (the X/Y/A/B buttons and the menu button)
	 *
	 * Note: no documentation I could find on this, found it by running SteamVR and noting these values down
	 *
	 * @author ZNix
	 * @since OpenVR 1.10.30
	 */
	char rchInputSourceType[32];
};

/** A hand's touches as the estimated skeleton shows them: the thumb and the index finger each move between lifted (0)
 * and touching (1) at kSpeed a second, so a touch doesn't snap the finger. Each hand has its own, under its own lock. */
struct HandTouchEase {
	static constexpr float kSpeed = 8.0f;
	using Clock = std::chrono::steady_clock;

	struct Eased {
		float thumb = 0.0f, trigger = 0.0f;
	};

	/** Moves towards the touches by the time since the last step (none on the first) and returns where they are */
	Eased Step(bool thumbTouch, bool triggerTouch, Clock::time_point now);

private:
	std::mutex lock;
	std::optional<Clock::time_point> last;
	Eased eased;
};

class BaseInput {
public:
	// Ctor/dtor
	BaseInput();
	~BaseInput();

	static void setStartupManifest(std::string manifest);

public:
	typedef vr::VRInputValueHandle_t VRInputValueHandle_t;
	typedef vr::EVRInputError EVRInputError;
	typedef vr::VRActionSetHandle_t VRActionSetHandle_t;
	typedef vr::VRActionHandle_t VRActionHandle_t;
	typedef vr::ETrackingUniverseOrigin ETrackingUniverseOrigin;
	typedef vr::EVRSkeletalMotionRange EVRSkeletalMotionRange;
	typedef vr::VRBoneTransform_t VRBoneTransform_t;
	typedef OOVR_VRActiveActionSet_t VRActiveActionSet_t;
	typedef OOVR_InputDigitalActionData_t InputDigitalActionData_t;
	typedef OOVR_InputAnalogActionData_t InputAnalogActionData_t;
	typedef OOVR_InputPoseActionData_t InputPoseActionData_t;
	typedef OOVR_InputSkeletalActionData_t InputSkeletalActionData_t;
	typedef OOVR_EVRSkeletalTransformSpace EVRSkeletalTransformSpace;
	typedef OOVR_InputOriginInfo_t InputOriginInfo_t;
	typedef OOVR_EVRSkeletalReferencePose EVRSkeletalReferencePose;
	typedef OOVR_EVRSummaryType EVRSummaryType;
	typedef OOVR_VRSkeletalSummaryData_t VRSkeletalSummaryData_t;

	XrHandTrackerEXT handTrackers[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

	/** With XR_HTCX_vive_tracker_interaction: the one pose action of every tracker role (each role's path is one of
	 * its subaction paths), in OpenLamina's own action set; null without it or before the actions are made */
	XrAction TrackerPoseAction() const { return trackerPoseActionAttached ? trackerPoseAction : XR_NULL_HANDLE; }
	/** Counts the tracker actions made: a new one needs new spaces */
	uint64_t TrackerPoseActionGeneration() const { return trackerPoseActionGeneration; }

	// ---------------  Handle management   --------------- //

	/** Sets the path to the action manifest JSON file that is used by this application. If this information
	 * was set on the Steam partner site, calls to this function are ignored. If the Steam partner site
	 * setting and the path provided by this call are different, VRInputError_MismatchedActionManifest is returned.
	 * This call must be made before the first call to UpdateActionState or IVRSystem::PollNextEvent. */
	EVRInputError SetActionManifestPath(const char* pchActionManifestPath);

	/** Returns a handle for an action set. This handle is used for all performance-sensitive calls. */
	EVRInputError GetActionSetHandle(const char* pchActionSetName, VRActionSetHandle_t* pHandle);

	/** Returns a handle for an action. This handle is used for all performance-sensitive calls. */
	EVRInputError GetActionHandle(const char* pchActionName, VRActionHandle_t* pHandle);

	/** Returns a handle for any path in the input system. E.g. /user/hand/right */
	EVRInputError GetInputSourceHandle(const char* pchInputSourcePath, VRInputValueHandle_t* pHandle);

	// --------------- Reading action state ------------------- //

	/** Reads the current state into all actions. After this call, the results of Get*Action calls
	 * will be the same until the next call to UpdateActionState. */
	EVRInputError UpdateActionState(VR_ARRAY_COUNT(unSetCount) VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount);

	/** UpdateActionState's argument check: a null or wrong-sized struct is an argument error, as SteamVR answers, and
	 * so is an entry without a set among several (whatever their priorities). VRInputError_None if they're usable. */
	static EVRInputError CheckActiveSets(const VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount)
	{
		if (sizeof(*pSets) != unSizeOfVRSelectedActionSet_t || (unSetCount && !pSets))
			return vr::VRInputError_InvalidParam;
		if (unSetCount > 1) {
			for (uint32_t i = 0; i < unSetCount; i++) {
				if (pSets[i].ulActionSet == vr::k_ulInvalidActionSetHandle)
					return vr::VRInputError_InvalidParam;
			}
		}
		return vr::VRInputError_None;
	}

	/** An action's fUpdateTime: when its value last changed (changeTime, 0 for never), as seconds from now (0 or
	 * less). One that never changed counts from when it was made (createdAt). */
	static float UpdateTimeFromNow(double changeTime, double createdAt, double now)
	{
		double since = changeTime != 0 ? changeTime : createdAt;
		return since >= now ? 0.0f : (float)(since - now);
	}

	/** Reads the state of a digital action given its handle. This will return VRInputError_WrongType if the type of
	 * action is something other than digital */
	EVRInputError GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of an analog action given its handle. This will return VRInputError_WrongType if the type of
	 * action is something other than analog */
	EVRInputError GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/**
	 * Reads the state of a pose action given its handle.
	 *
	 * Renamed to GetPoseActionDataRelativeToNow as of IVRInput_006.
	 */
	EVRInputError GetPoseActionData(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of a pose action given its handle for the number of seconds relative to now. This
	 * will generally be called with negative times from the fUpdateTime fields in other actions. */
	EVRInputError GetPoseActionDataRelativeToNow(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow, InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of a pose action given its handle. The returned values will match the values returned
	 * by the last call to IVRCompositor::WaitGetPoses(). */
	EVRInputError GetPoseActionDataForNextFrame(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, InputPoseActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of a skeletal action given its handle. */
	EVRInputError GetSkeletalActionData(VRActionHandle_t action, InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of a skeletal action given its handle. */
	EVRInputError GetSkeletalActionData(VRActionHandle_t action, InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize);

	/** IVRInput_003 (SDK 1.0.15): before ulRestrictToDevice, so any device */
	EVRInputError GetDigitalActionData(VRActionHandle_t action, InputDigitalActionData_t* pActionData, uint32_t unActionDataSize);
	EVRInputError GetAnalogActionData(VRActionHandle_t action, InputAnalogActionData_t* pActionData, uint32_t unActionDataSize);
	EVRInputError GetPoseActionData(VRActionHandle_t action, ETrackingUniverseOrigin eOrigin, float fPredictedSecondsFromNow,
	    InputPoseActionData_t* pActionData, uint32_t unActionDataSize);
	EVRInputError TriggerHapticVibrationAction(VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds,
	    float fFrequency, float fAmplitude);

	/** IVRInput_003: the action data and its bones in one call (as GetSkeletalActionData and GetSkeletalBoneData,
	 * with the controller's range of motion) */
	EVRInputError GetSkeletalActionData(VRActionHandle_t action, EVRSkeletalTransformSpace eBoneParent, float fPredictedSecondsFromNow,
	    InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray,
	    uint32_t unTransformArrayCount);

	/** Returns the current dominant hand for the user for this application. This function will only return success for applications
	 * which include "supports_dominant_hand_setting": true in their action manifests. The dominant hand will only change after
	 * a call to UpdateActionState, and the action data returned after that point will use the new dominant hand. */
	EVRInputError GetDominantHand(vr::ETrackedControllerRole* peDominantHand);

	/** Eye tracking (OpenVR 2.15). */
	EVRInputError GetEyeTrackingDataRelativeToNow(VRActionHandle_t action, vr::ETrackingUniverseOrigin eOrigin,
	    float fPredictedSecondsFromNow, vr::VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize);
	EVRInputError GetEyeTrackingDataForNextFrame(VRActionHandle_t action, vr::ETrackingUniverseOrigin eOrigin,
	    vr::VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize);

	/** Sets the dominant hand for the user for this application. */
	EVRInputError SetDominantHand(vr::ETrackedControllerRole eDominantHand);

	// ---------------  Static Skeletal Data ------------------- //

	/** Reads the number of bones in skeleton associated with the given action */
	EVRInputError GetBoneCount(VRActionHandle_t action, uint32_t* pBoneCount);

	/** Fills the given array with the index of each bone's parent in the skeleton associated with the given action */
	EVRInputError GetBoneHierarchy(VRActionHandle_t action, VR_ARRAY_COUNT(unIndexArayCount) vr::BoneIndex_t* pParentIndices, uint32_t unIndexArayCount);

	/** Fills the given buffer with the name of the bone at the given index in the skeleton associated with the given action */
	EVRInputError GetBoneName(VRActionHandle_t action, vr::BoneIndex_t nBoneIndex, VR_OUT_STRING() char* pchBoneName, uint32_t unNameBufferSize);

	/** Fills the given buffer with the transforms for a specific static skeletal reference pose */
	EVRInputError GetSkeletalReferenceTransforms(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalReferencePose eReferencePose, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount);

	/** Reads the level of accuracy to which the controller is able to track the user to recreate a skeletal pose */
	EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t action, vr::EVRSkeletalTrackingLevel* pSkeletalTrackingLevel);

	// ---------------  Dynamic Skeletal Data ------------------- //

	/** Reads the state of the skeletal bone data associated with this action and copies it into the given buffer. */
	EVRInputError GetSkeletalBoneData(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of the skeletal bone data associated with this action and copies it into the given buffer. */
	EVRInputError GetSkeletalBoneData(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount);

	/** Reads summary information about the current pose of the skeleton associated with the given action.   */
	EVRInputError GetSkeletalSummaryData(VRActionHandle_t action, EVRSummaryType eSummaryType, VRSkeletalSummaryData_t* pSkeletalSummaryData);

	// Same as above, but default to VRSummaryType_FromDevice (TODO check if this matches SteamVR). This was used in IVRInput_005
	EVRInputError GetSkeletalSummaryData(VRActionHandle_t action, VRSkeletalSummaryData_t* pSkeletalSummaryData);

	/** Reads the state of the skeletal bone data in a compressed form that is suitable for
	 * sending over the network. The required buffer size will never exceed ( sizeof(VR_BoneTransform_t)*boneCount + 2).
	 * Usually the size will be much smaller. */
	EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t action, EVRSkeletalTransformSpace eTransformSpace, EVRSkeletalMotionRange eMotionRange, VR_OUT_BUFFER_COUNT(unCompressedSize) void* pvCompressedData, uint32_t unCompressedSize, uint32_t* punRequiredCompressedSize, VRInputValueHandle_t ulRestrictToDevice);

	/** Reads the state of the skeletal bone data in a compressed form that is suitable for
	 * sending over the network. The required buffer size will never exceed ( sizeof(VR_BoneTransform_t)*boneCount + 2).
	 * Usually the size will be much smaller. */
	EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t action, EVRSkeletalMotionRange eMotionRange, VR_OUT_BUFFER_COUNT(unCompressedSize) void* pvCompressedData, uint32_t unCompressedSize, uint32_t* punRequiredCompressedSize);

	/** IVRInput_003 (SDK 1.0.15): the compressed bone data calls before they were renamed; as today's */
	EVRInputError GetSkeletalActionDataCompressed(VRActionHandle_t action, EVRSkeletalTransformSpace eBoneParent, float fPredictedSecondsFromNow,
	    VR_OUT_BUFFER_COUNT(unCompressedSize) void* pvCompressedData, uint32_t unCompressedSize, uint32_t* punRequiredCompressedSize);
	EVRInputError UncompressSkeletalActionData(void* pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace* peBoneParent,
	    VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount);

	/** Turns a compressed buffer from GetSkeletalBoneDataCompressed and turns it back into a bone transform array. */
	EVRInputError DecompressSkeletalBoneData(void* pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace* peTransformSpace, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount);

	/** Turns a compressed buffer from GetSkeletalBoneDataCompressed and turns it back into a bone transform array. */
	EVRInputError DecompressSkeletalBoneData(const void* pvCompressedBuffer, uint32_t unCompressedBufferSize, EVRSkeletalTransformSpace eTransformSpace, VR_ARRAY_COUNT(unTransformArrayCount) VRBoneTransform_t* pTransformArray, uint32_t unTransformArrayCount);

	// --------------- Haptics ------------------- //

	/** Triggers a haptic event as described by the specified action */
	EVRInputError TriggerHapticVibrationAction(VRActionHandle_t action, float fStartSecondsFromNow, float fDurationSeconds, float fFrequency, float fAmplitude, VRInputValueHandle_t ulRestrictToDevice);

	// --------------- Action Origins ---------------- //

	/** Retrieve origin handles for an action */
	EVRInputError GetActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t digitalActionHandle, VR_ARRAY_COUNT(originOutCount) VRInputValueHandle_t* originsOut, uint32_t originOutCount);

	/** Retrieves the name of the origin in the current language */
	EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, VR_OUT_STRING() char* pchNameArray, uint32_t unNameArraySize);

	/** Retrieves the name of the origin in the current language. unStringSectionsToInclude is a bitfield of values in EVRInputStringBits that allows the
	        application to specify which parts of the origin's information it wants a string for. */
	EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, VR_OUT_STRING() char* pchNameArray, uint32_t unNameArraySize, uint64_t unStringSectionsToInclude);

	/** Retrieves useful information for the origin of this action */
	EVRInputError GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t* pOriginInfo, uint32_t unOriginInfoSize);

	/** Retrieves useful information about the bindings for an action */
	EVRInputError GetActionBindingInfo(VRActionHandle_t action, OOVR_InputBindingInfo_t* bindingInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount, uint32_t* punReturnedBindingInfoCount);

	/** Shows the current binding for the action in-headset */
	EVRInputError ShowActionOrigins(VRActionSetHandle_t actionSetHandle, VRActionHandle_t ulActionHandle);

	/** Shows the current binding all the actions in the specified action sets */
	EVRInputError ShowBindingsForActionSet(VR_ARRAY_COUNT(unSetCount) VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount, VRInputValueHandle_t originToHighlight);

	/** Use this to query what action on the component returned by GetOriginTrackedDeviceInfo would trigger this binding. */
	EVRInputError GetComponentStateForBinding(const char* pchRenderModelName, const char* pchComponentName,
	    const OOVR_InputBindingInfo_t* pOriginInfo, uint32_t unBindingInfoSize, uint32_t unBindingInfoCount,
	    vr::RenderModel_ComponentState_t* pComponentState);

	// --------------- Legacy Input ------------------- //
	bool IsUsingLegacyInput();

	// --------------- Utilities ------------------- //

	/** Opens the binding user interface. If no app key is provided it will use the key from the calling process.
	 * If no set is provided it will open to the root of the app binding page. */
	EVRInputError OpenBindingUI(const char* pchAppKey, VRActionSetHandle_t ulActionSetHandle, VRInputValueHandle_t ulDeviceHandle, bool bShowOnDesktop);

	/**
	 * Returns the variant set in the current bindings. If the binding doesn't include a variant setting, this function
	 * will return an empty string
	 */
	EVRInputError GetBindingVariant(vr::VRInputValueHandle_t ulDevicePath, char* pchVariantArray, uint32_t unVariantArraySize);

public: // INTERNAL FUNCTIONS
	/** Each finger's bones from parent space into model space (the wrist and aux bones are the same in both) */
	static void ParentSpaceSkeletonToModelSpace(VRBoneTransform_t* joints);

	/**
	 * Converts located hand joints to the 31 bones of an OpenVR hand, in parent space. Returns false, leaving
	 * the output only partly written, when a joint it needs has no valid pose.
	 */
	static bool XrHandJointsToSkeleton(const std::vector<XrHandJointLocationEXT>& joints, bool isRight, VRBoneTransform_t* output, glm::mat4 transform);

	/** Clears a caller's bone array; does nothing for a null array. */
	static void ClearBones(VRBoneTransform_t* bones, uint32_t count);

	/** The error to answer for a bone array that cannot take a whole skeleton, or None. */
	static EVRInputError CheckBoneArray(const VRBoneTransform_t* bones, uint32_t count);

	/** Whether a hand is one of the two a skeleton can belong to (an action that is not a skeleton has none). */
	static bool IsSkeletonHand(ITrackedDevice::TrackedDeviceType hand);
	/**
	 * Bind all the inputs to the current OpenXR session. This must be called after swapping the session to keep
	 * the inputs working.
	 */
	void BindInputsForSession();

	/**
	 * Similar to setting the manifest, but doesn't actually load one. Equivalent to passing in a blank manifest.
	 *
	 * If a manifest is already loaded, this does nothing. It should be called when a function from the legacy
	 * input system is used. That way, games that belatedly load their manifests won't be mis-identified as using
	 * the legacy system (previously this was called after the first frame).
	 *
	 * Used for games that don't use the input system.
	 */
	void LoadEmptyManifestIfRequired();

	/**
	 * Update the input stuff, called by BaseSystem.
	 *
	 * Currently, it updates the OpenXR stuff if the game is running in legacy mode.
	 */
	void InternalUpdate();

	/**
	 * Called to implement BaseSystem::GetControllerState. The documentation for that reads:
	 *
	 * Fills the supplied struct with the current state of the controller. Returns false if the controller index
	 * is invalid.
	 */
	bool GetLegacyControllerState(vr::TrackedDeviceIndex_t controllerDeviceIndex, vr::VRControllerState_t* controllerState);

	void TriggerLegacyHapticPulse(vr::TrackedDeviceIndex_t controllerDeviceIndex, uint64_t durationNanos);

	// aimPose defaults to false (grip), since OpenVR games are typically expecting a "raw"/natural controller pose, and
	// the grip pose is the closest analog to that.
	void GetHandSpace(vr::TrackedDeviceIndex_t index, XrSpace& space, bool aimPose = false);

	void GetHandSpace(ITrackedDevice::TrackedDeviceType hand, XrSpace& space, bool aimPose);

	bool AreActionsLoaded();

	/** One hand's laser for pointing at overlays (LaserMouse) */
	struct HandPointer {
		XrPosef aim; // the aim pose: the laser runs along its -Z
		float trigger; // 0 to 1
		float stickX, stickY; // -1 to 1
	};
	/**
	 * One hand's (0 the left, 1 the right) pointer: its aim pose located in base at time, its trigger and its
	 * thumbstick (or trackpad), as of the last sync, from the raw inputs. False before any actions are loaded, or while
	 * the hand's aim isn't tracked.
	 * Takes the session lock (shared), then evaluatorLock.
	 */
	bool GetHandPointer(int hand, XrSpace base, XrTime time, HandPointer& out);
	/**
	 * Syncs the old input's actions (and the raw inputs) alone: for a call that blocks the game, which then syncs
	 * none of its own (ShowMessageOverlay). Nothing before any actions are loaded.
	 */
	void SyncPointerActions();
	/**
	 * Whether the button that opens the dashboard is held on a hand (0 the left, 1 the right), as of the last sync.
	 * system: that controller's system button, where the runtime hands it to applications. Otherwise the left
	 * controller's menu button (never the right's), from the raw inputs. False before any actions are loaded.
	 */
	bool GetDashboardButton(bool system, int hand);
	/**
	 * While OpenLamina's dashboard is open, the game's input is held: its digital and analog actions read inactive, and
	 * its old controller states zero
	 */
	static void SetHeldByDashboard(bool held);
	static bool HeldByDashboard();
	/**
	 * Whether the game's input is held: by the dashboard, or while the compositor's grid is fully faded in (the game
	 * is loading, and the user sees the grid, not the game). Held, its digital and analog actions read inactive and its
	 * old controller states zero.
	 */
	static bool InputHeld();
	/** A vibration on one hand (0 the left, 1 the right), through the old input's haptic action */
	void TriggerHandHaptic(int hand, float seconds, float frequency, float amplitude);

	/**
	 * The eye gaze (XR_EXT_eye_gaze_interaction) located in a space at a time: false without a gaze (no extension,
	 * no eye tracker, actions not loaded yet) or while it isn't valid. Its -Z is the direction the eyes look in.
	 * flags, if given, gets the location's flags. Call with the session locked (shared).
	 */
	bool GetEyeGaze(XrSpace base, XrTime time, XrPosef* gaze, XrSpaceLocationFlags* flags = nullptr);
	// Whether there is an eye gaze to ask GetEyeGaze for, valid or not
	bool HasEyeGaze();

	/**
	 * Get a number that increments each time xrSyncActions is called. Can be used to check if a cached input value
	 * is current or not.
	 */
	inline uint64_t GetSyncSerial() const { return syncSerial; }

private:
	inline static std::string startupManifest;

public:
	enum class ActionType {
		Boolean = 0,
		Vector1,
		Vector2,
		Vector3,
		Vibration,
		Pose,
		Skeleton,
		EyeTracking,
	};

	/** Which hands (0 left, 1 right) an active action set is on, and which its secondary set is on. restrictedHand is
	 * -1 when the set isn't restricted, 0 or 1 for a hand, and 2 for a device that isn't a hand (the set is then on
	 * no hand, and the secondary set is on both: the secondary set is for every device but the restricted one). */
	struct SetHands {
		bool primary[2] = { false, false };
		bool secondary[2] = { false, false };
	};
	static SetHands HandsForActiveSet(int restrictedHand)
	{
		SetHands hands;
		if (restrictedHand < 0) {
			hands.primary[0] = hands.primary[1] = true;
		} else if (restrictedHand < 2) {
			hands.primary[restrictedHand] = true;
			hands.secondary[1 - restrictedHand] = true;
		} else {
			hands.secondary[0] = hands.secondary[1] = true;
		}
		return hands;
	}

	/** GetDigitalActionData's answer for an action of this type: only a boolean action is read as digital. */
	static EVRInputError DigitalReadError(ActionType type)
	{
		return type == ActionType::Boolean ? vr::VRInputError_None : vr::VRInputError_WrongType;
	}

	/** A haptic pulse's duration for a game's seconds: the shortest the runtime offers for none (or a bad value), and
	 * never past an hour, so the nanoseconds fit the 64-bit count. */
	static XrDuration HapticDuration(float seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0)
			return XR_MIN_HAPTIC_DURATION;
		return (XrDuration)((std::min)((double)seconds, 3600.0) * 1e9);
	}

	/** A haptic pulse's amplitude: the game's, scaled by its binding, kept within OpenXR's 0 to 1. */
	static float HapticAmplitude(float amplitude, float scale)
	{
		float value = amplitude * scale;
		return std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, 1.0f);
	}

private:
	enum class ActionSetUsage {
		LeftRight = 0, // User can bind each side separately
		Single, // What's bound to one controller is bound to the other
		Hidden, // Not shown to the user
	};

	// Represents an action set. This is a set of controls that can be configured
	// independently - as I understand it, these are to be used for different portions
	// of a game. You might have one action set for shooting, one for driving, and so on.
	// It also stores the usage mode, which controls how it should eventually be shown
	// in the binding editor (whether the user can bind controls separately for each hand
	// or not).
	// See https://github.com/ValveSoftware/openvr/wiki/Action-manifest#action-sets
	struct ActionSet {
		std::string fullName; // Eg '/actions/main'
		std::string name; // Eg 'main'
		std::string manifestName; // As the manifest has it, lower-cased and never shortened (the binding evaluator's name)
		ActionSetUsage usage = ActionSetUsage::LeftRight;

		XrActionSet xr = XR_NULL_HANDLE;
	};

	enum class PoseBindingPoint {
		RAW, // From /raw or /aim - this is the pose returned by WaitGetPoses
		BASE, // This is the pose from the 'base' component
		GRIP, // This is the pose from the 'grip' component, matching the OpenXR grip action
		HANDGRIP, // This is the pose from the 'handgrip' component, matching the OpenXR grip action
		TIP, // This is the pose from the 'tip' component
		BODY, // This is the pose from the 'body' component
		GDC2015, // This is the pose from the 'gdc2015' component
	};

	struct PoseBindingInfo {
		PoseBindingPoint point = PoseBindingPoint::RAW;
		ITrackedDevice::TrackedDeviceType hand = ITrackedDevice::HAND_LEFT;
	};

	struct Action {
	private:
		static constexpr size_t sources_size = 32;

	public:
		~Action(); // Must be defined non-inline to avoid it ending up in stubs.gen.cpp

		// Since the list of VirtualInputs cannot be copied (only moved) we might as well make the
		// whole struct non-copyable to make the error messages easier to follow.
		Action() = default;
		Action(const Action&) = delete;

		std::string fullName; // Full name as set in the JSON file, eg '/actions/main/in/Test1'
		std::string shortName; // The last part of the name, eg 'Test1'
		std::string manifestName; // As the manifest has it, lower-cased and never shortened (the binding evaluator's name)
		bool haptic = false;
		ActionType type = ActionType::Boolean;
		ActionSet* set = nullptr;
		std::string setName; // The name of the action set - set before we've enumerated the action sets, eg 'main'
		double createdAt = 0; // when the manifest made it, on the evaluator's clock (its fUpdateTime until it changes)

		XrAction xr = XR_NULL_HANDLE;
		// If this is a skeletal action, what hand it's bound to - this is set in the actions
		// manifest itself, not a binding file.
		ITrackedDevice::TrackedDeviceType skeletalHand = ITrackedDevice::HAND_NONE;

		// Only used in the case of Pose actions, this is the action space for each subaction path
		// The indexes match up with allSubactionPaths
		std::vector<XrSpace> actionSpaces;

		// If this is a pose action, we calculate it from the legacy pose inputs rather than giving it
		// it's own OpenXR action, since it may need some special transforms to make it line up properly.
		std::unordered_map<const InteractionProfile*, PoseBindingInfo> poseBindingsLeft, poseBindingsRight;
		// Those on a hand named by dominance (/user/hand/primary/pose/raw), on whichever hand that is when it's read
		std::unordered_map<const InteractionProfile*, PoseBindingInfo> poseBindingsPrimary, poseBindingsSecondary;
	};

public:
	enum class InputSource {
		INVALID,
		HAND_LEFT,
		HAND_RIGHT,
		HEAD,
		OTHER, // a device that isn't a hand or the head (gamepad, tracker...): it matches no hand
	};

	/** What a handle's path asks for. A hand named by dominance (/user/hand/primary) follows the dominant hand. */
	enum class HandAlias {
		NONE,
		PRIMARY,
		SECONDARY,
	};

	struct InputSourceInfo {
		InputSource type = InputSource::INVALID;
		HandAlias alias = HandAlias::NONE;
		std::string devicePath; // the device's own path: /user/hand/left, /user/head...
	};
	static bool PathIsOrBelow(const std::string& path, const std::string& prefix)
	{
		return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
	}
	/** What an input source path names: a hand (left or right, or by dominance), the head, another device, or nothing
	 * the runtime knows. The path is the device's own or one of its inputs below it. */
	static InputSourceInfo ClassifyInputSource(const std::string& path)
	{
		InputSourceInfo info;
		if (PathIsOrBelow(path, "/user/hand/left")) {
			info.type = InputSource::HAND_LEFT;
			info.devicePath = "/user/hand/left";
		} else if (PathIsOrBelow(path, "/user/hand/right")) {
			info.type = InputSource::HAND_RIGHT;
			info.devicePath = "/user/hand/right";
		} else if (PathIsOrBelow(path, "/user/hand/primary")) {
			info.type = InputSource::HAND_RIGHT; // the hand it is pointed at is picked from the dominant hand
			info.alias = HandAlias::PRIMARY;
			info.devicePath = "/user/hand/primary";
		} else if (PathIsOrBelow(path, "/user/hand/secondary")) {
			info.type = InputSource::HAND_LEFT;
			info.alias = HandAlias::SECONDARY;
			info.devicePath = "/user/hand/secondary";
		} else if (PathIsOrBelow(path, "/user/head")) {
			info.type = InputSource::HEAD;
			info.devicePath = "/user/head";
		} else {
			for (const char* other : { "/user/gamepad", "/user/treadmill", "/user/eyes" }) {
				if (PathIsOrBelow(path, other)) {
					info.type = InputSource::OTHER;
					info.devicePath = other;
					return info;
				}
			}
			// A tracker by the path SteamVR binds its pose on: /user/waist, /user/foot/left
			if (std::optional<ViveTrackerRole> role = ViveTrackerRoleForSteamVRPath(path)) {
				info.type = InputSource::OTHER;
				info.devicePath = role->steamvrPath;
				return info;
			}
			// Trackers are named by their role or serial: /user/vive_tracker_htcx/role/waist
			static const std::string trackerPrefix = "/user/vive_tracker";
			if (path.starts_with(trackerPrefix)) {
				size_t end = path.find('/', 1 + std::string("/user").size());
				info.type = InputSource::OTHER;
				info.devicePath = path.substr(0, end);
			}
		}
		return info;
	}

private:

	struct InputValueHandle {
		InputValueHandle();
		~InputValueHandle();

		/**
		 * The full path of this handle, eg /user/hand/left/input/select/value.
		 *
		 * This is what was passed to GetInputSourceHandle.
		 */
		std::string path;

		/**
		 * If this is a valid input source (eg in /user/hand/left or a child thereof) this specifies
		 * what that input source is.
		 */
		InputSource type = InputSource::INVALID;
		HandAlias alias = HandAlias::NONE;

		/**
		 * If this is a valid input source, this specifies the path of the input device it is mounted on as
		 * an OpenXR path atom. If it is not, this is XR_NULL_PATH.
		 *
		 * This will match one of the paths from allSubactionPaths.
		 *
		 * Eg /user/hand/left.
		 */
		XrPath devicePath = XR_NULL_PATH;

		/**
		 * A string version of #devicePath. Null corresponds to an empty string.
		 */
		std::string devicePathString;
	};

	using RegHandle = uint64_t;

	// For keeping track of string-lookup based stuff: actions, actionsets, and inputvaluehandles.
	// These basically serve two purposes: storing useful stuff that OpenComposite inserts, and associating
	//  a name the game made up and we don't support with a consistent handle.
	// Think of this as being used for something like XrPath: the application can convert a string into a
	//  handle and that handle must be both unique for that string, and constant across calls with the same
	//  string supplied.
	// Additionally, some magic strings that OpenComposite loads are associated with additional data.
	// These provide an opaque handle for a given string. If that string has an object associated with it then
	//  it's that pointer (to make debugging easier, since you just cast to inspect the contents) or a dummy
	//  value of a constant plus some randomisation otherwise.
	// Handle-to-object lookups are always done through an unsorted map (no pointer cases, just in case the
	//  memory behind the dummy values are somehow allocated and the performance cost should be very
	//  small), the handle being the pointer solely for debugging.
	template <typename T>
	class Registry {
	public:
		Registry(uint32_t _maxNameSize);
		~Registry() = default;

		T* LookupItem(const std::string& name) const;
		T* LookupItem(RegHandle handle) const;
		RegHandle LookupHandle(const std::string& name);
		T* Initialise(const std::string& name, std::unique_ptr<T> value);
		std::vector<std::unique_ptr<T>>& GetItems() { return storage; }

		// Function for shortening or looking up a shortened version of a name (if it exists)
		// Necessary because OpenXR has defined limits on name lengths, while OpenVR appears to have no such limits
		std::string ShortenOrLookupName(const std::string& longName);
		void Reset();

	private:
		// A map of names to handles, used in the common case of not-the-first call
		std::unordered_map<std::string, RegHandle> handlesByName;

		// Reverse-lookup for debugging
		std::unordered_map<RegHandle, std::string> namesByHandle;

		// Handles to their associated item, or nothing
		std::unordered_map<RegHandle, T*> itemsByHandle;

		// Also for bonus performance, name to handle directly. Access time is more
		// important than memory usage here, as there won't be many actual items (maybe
		// single-digit thousands at most).
		std::unordered_map<std::string, T*> itemsByName;

		// The storage for all the actual items
		std::vector<std::unique_ptr<T>> storage;

		// A map for names that are too long.
		// For actions, these are names longer than XR_MAX_ACTION_NAME_SIZE
		// For action sets, XR_MAX_ACTION_SET_NAME_SIZE
		std::unordered_map<std::string, std::string> longNames;

		// The maximum name size. Does not include the null terminator.
		const uint32_t maxNameSize;
	};

	// See GetSyncSerial
	std::atomic<uint64_t> syncSerial = 0;

	// What each hand's legacy controller state was when it last changed, and its packet number then
	struct LegacyPacket {
		vr::VRControllerState_t last = {};
		uint32_t packet = 0;
		bool valid = false;
	};
	std::mutex legacyPacketLock;
	LegacyPacket legacyPackets[2];

	// Set under the exclusive session lock; read without it on the way in to LoadEmptyManifestIfRequired
	std::atomic<bool> hasLoadedActions = false;
	std::string loadedActionsPath;
	std::mutex loadedActionsPathLock; // read before the session lock, to answer a repeated manifest at once
	bool usingLegacyInput = false;
	Registry<ActionSet> actionSets;
	Registry<Action> actions;
	bool allowSetDominantHand = false;
	// True while we are in the middle of a session restart request
	// Necessary for BindInputsForSession (see comments there)
	bool restartingSession = false;

	std::atomic<vr::ETrackedControllerRole> dominantHand = vr::TrackedControllerRole_RightHand;
	std::atomic<vr::ETrackedControllerRole> requestedDominantHand = vr::TrackedControllerRole_RightHand; // taken up at the next UpdateActionState

	// TODO convert to Registry
	std::unordered_map<std::string, std::unique_ptr<InputValueHandle>> inputHandleRegistry;
	std::mutex inputHandleRegistryLock; // GetInputSourceHandle is called from any thread
	/** Points a handle at the left (0) or right (1) hand */
	void PointHandleAtHand(InputValueHandle& handle, int hand);
	/** Points the handles named by dominance at the dominant hand's side */
	void ResolveHandAliases();
	/** The hand (0 left, 1 right) a handle named by dominance points at */
	int DominantHandIndex(HandAlias alias) const;

	XrActionSet legacyInputsSet = XR_NULL_HANDLE;

	// In the legacy set when XR_HTCX_vive_tracker_interaction is enabled: every tracker role's grip pose
	XrAction trackerPoseAction = XR_NULL_HANDLE;
	bool trackerPoseActionAttached = false;
	uint64_t trackerPoseActionGeneration = 0;
	// The pose actions (manifest names, lower-cased) the game's binding files for trackers bind, and the roles they
	// bind them on (waist, left_foot...)
	std::map<std::string, std::vector<std::string>> trackerPoseRoutes;
	/** A pose action's data from the tracker whose role a tracker's binding file binds it on; false if none does */
	bool TrackerPoseData(const std::string& action, vr::ETrackingUniverseOrigin origin, const InputValueHandle* restrictToDevice,
	    InputPoseActionData_t* data);

	// In the legacy set when XR_EXT_eye_gaze_interaction is enabled: the gaze pose, and its space in this session
	XrAction eyeGazeAction = XR_NULL_HANDLE;
	XrSpace eyeGazeSpace = XR_NULL_HANDLE;

	// An action set with one action per raw input (by its OpenXR component, such as /input/trigger/click, for every
	// hand), each profile's raw inputs and its binding file's evaluator, and the evaluators the last UpdateActionState
	// updated
	XrActionSet rawInputsSet = XR_NULL_HANDLE;
	std::map<std::string, XrAction> rawActions;
	std::map<const InteractionProfile*, std::vector<std::pair<std::string, std::string>>> rawComponents;
	std::map<const InteractionProfile*, std::unique_ptr<BindingEvaluator>> evaluators;
	std::vector<const BindingEvaluator*> evaluatedThisSync;
	// Each profile's binding file: what GetActionBindingInfo and GetActionOrigins answer
	std::map<const InteractionProfile*, BindingFile> bindingFiles;
	// The binding files that name a hand by dominance, as they are, with the controller type they're used for:
	// bindingFiles has them on the dominant hand
	struct AliasedBindingFile {
		BindingFile file;
		std::string controllerType;
	};
	std::map<const InteractionProfile*, AliasedBindingFile> aliasedBindingFiles;
	/** Puts the files of aliasedBindingFiles, and their evaluators, on the dominant hand. Call it with evaluatorLock held. */
	void ResolveBindingFileHands();
	// Guards everything above and the legacy evaluators: a game reads input on its own thread while OpenComposite
	// reads the old input after each frame on the thread that submits.
	// Lock order: the session lock (xr_session, shared or exclusive) is taken before evaluatorLock, never after. Every
	// reader of input holds the session lock shared, so a session restart (exclusive) waits for it; a thread holding
	// evaluatorLock that then asked for the session lock could wait behind a restart that waits for a reader that
	// waits for evaluatorLock.
	mutable std::recursive_mutex evaluatorLock;
	void CreateRawActions();
	void SuggestRawInputs(const InteractionProfile& profile, std::vector<XrActionSuggestedBinding>& bindings);
	/** Suggests the profile's raw inputs and makes its binding file's evaluator, if the file binds an input the
	 * controller has to one of the manifest's actions or bindings holds a haptic; false (nothing done) if not */
	bool SuggestRawBindings(const InteractionProfile& profile, const std::string& bindingsPath, std::vector<XrActionSuggestedBinding>& bindings);
	/** An input action's bindings on the controllers in use, from their binding files; nothing if no controller in
	 * use has one, or for a pose, a skeleton or a vibration. Takes evaluatorLock. */
	std::optional<std::vector<ActionBinding>> BindingsFromFiles(const Action& action);
	/** One hand's raw inputs (index into allSubactionPaths) on a profile, by SteamVR path, into state */
	void ReadRawInputs(size_t hand, const InteractionProfile& profile, RawInputState& state);
	/** The old input's evaluator for a controller's profile, evaluated once per sync; null if it has none.
	 * Call it, and use what it returns, with evaluatorLock held. */
	const BindingEvaluator* LegacyEvaluatorFor(const InteractionProfile* profile);
	struct LegacyEvaluator {
		std::unique_ptr<BindingEvaluator> evaluator;
		uint64_t evaluatedAt = 0;
		bool evaluated = false;
	};
	// By profile: profiles of one controller type (the Touch models) read their own hands
	std::map<const InteractionProfile*, LegacyEvaluator> legacyEvaluators;
	void UpdateEvaluators(const VRActiveActionSet_t* sets, uint32_t count);
	/** Writes a device's pose with a render model component's offset (none for a null or unknown name) into out */
	void ApplyPoseComponent(const vr::TrackedDevicePose_t& rawPose, ITrackedDevice::TrackedDeviceType hand, const char* componentName, vr::TrackedDevicePose_t& out);
	/** The eye tracking read at an XrTime (0: the frame being predicted) */
	EVRInputError GetEyeTrackingDataAt(VRActionHandle_t action, vr::ETrackingUniverseOrigin eOrigin, XrTime time,
	    vr::VREyeTrackingData_t* pEyeTrackingData, uint32_t ulEyeTrackingDataSize);
	/** An action set active for one frame, with the hands it's on */
	struct ActiveSetOnHands {
		ActionSet* set = nullptr;
		int32_t priority = 0;
		bool hands[2] = { false, false };
	};
	/** The sets a game's UpdateActionState named, with a restricted set on its hand and its secondary set on the
	 * others; a set named by a handle that isn't there is left out */
	std::vector<ActiveSetOnHands> ResolveActiveSets(const VRActiveActionSet_t* sets, uint32_t count);

	// Haptic pulses a game asked to start later, applied at the first sync once they're due
	struct PendingHaptic {
		double due = 0;
		XrAction action = XR_NULL_HANDLE;
		XrPath subactionPath = XR_NULL_PATH;
		XrDuration duration = 0;
		float frequency = 0;
		float amplitude = 0;
	};
	std::mutex hapticQueueLock;
	std::vector<PendingHaptic> pendingHaptics;
	void ApplyHaptic(const PendingHaptic& pulse);
	/** Applies the queued pulses that are due. Call it with the session lock held. */
	void FlushDueHaptics();
	/** This sync's digital or analog output for an action from the evaluators, matching ulRestrictToDevice (a copy,
	 * taken under evaluatorLock) */
	std::optional<DigitalOut> EvaluatedDigital(const Action& action, VRInputValueHandle_t restrictToDevice) const;
	std::optional<AnalogOut> EvaluatedAnalog(const Action& action, VRInputValueHandle_t restrictToDevice) const;
	/** The input source handle of a binding's source path (an evaluated action's origin) */
	VRInputValueHandle_t OriginHandle(const std::string& sourcePath);

	/**
	 * The list of subaction paths anything can be bound to - this basically just means 'everything' and contains
	 * both the left and right hand paths as defined in the OpenXR spec.
	 */
	std::vector<XrPath> allSubactionPaths;

	/**
	 * String version of allSubactionPaths.
	 */
	std::vector<std::string> allSubactionPathNames = {
		"/user/hand/left",
		"/user/hand/right",
	};

	std::vector<const InteractionProfile*> handInteractionProfiles;

	void LoadBindingsSet(const InteractionProfile& profile, const std::string& bindingsPath);

	void CreateLegacyActions();
	// Suggests a profile's bindings (suggested points at bindings); refused with the system button's among them, again
	// without those
	XrResult SuggestBindings(XrInteractionProfileSuggestedBinding& suggested, std::vector<XrActionSuggestedBinding>& bindings);

	/**
	 * Convert a tracked device index to 0=left 1=right -1=other
	 */
	static int DeviceIndexToHandId(vr::TrackedDeviceIndex_t idx);

	LegacyControllerActions legacyControllers[2] = {};

	// Utility functions
	Action* cast_AH(VRActionHandle_t);
	ActionSet* cast_ASH(VRActionSetHandle_t);
	static InputValueHandle* cast_IVH(VRInputValueHandle_t);
	static std::shared_ptr<ITrackedDevice> ivhToDev(VRInputValueHandle_t handle);
	static bool checkRestrictToDevice(vr::VRInputValueHandle_t restrict, XrPath subactionPath);
	static ITrackedDevice::TrackedDeviceType ParseAndRemoveHandPrefix(std::string& toModify);

	/**
	 * Uses the finger tracking extensions to generate a skeletal summary.
	 */
	EVRInputError getRealSkeletalSummary(ITrackedDevice::TrackedDeviceType hand, VRSkeletalSummaryData_t* pSkeletalSummaryData);

	/**
	 * Uses input state (trigger and grip) to generate a skeletal summary.
	 */
	EVRInputError getEstimatedSkeletalSummary(ITrackedDevice::TrackedDeviceType hand, VRSkeletalSummaryData_t* pSkeletalSummaryData);
	EVRInputError getEstimatedBoneData(ITrackedDevice::TrackedDeviceType hand, EVRSkeletalTransformSpace transformSpace, EVRSkeletalMotionRange eMotionRange, std::span<VRBoneTransform_t, eBone_Count> boneData);
	/** Each hand's touches for the estimated skeleton (left, right) */
	std::array<HandTouchEase, 2> handTouches;

	/**
	 * Some games (i.e. newer Unity games) won't explicitly call SetActionManifestPath, but instead will set the path through
	 * the Steamworks web interface. If this is the case, we'll never be able to set the actions! So instead, we must discover if this is the case.
	 * This method should only be called when a game is trying to retrieve action data (the GetXActionData family of functions).
	 */
	void checkIfActionsMustBeDiscoveredFromSteam();
	bool triedDiscoveringManifest = false;
};

// Valve's 31-bone hand skeleton, its hierarchy and names, and OpenComposite's compressed bone data
// (BaseInput_Hand.cpp)

/** Bone's parent (-1 for the root), as https://github.com/ValveSoftware/openvr/wiki/Hand-Skeleton has them */
vr::BoneIndex_t HandBoneParent(vr::BoneIndex_t bone);
/** Bone's name in SteamVR's glove skeleton, with the hand's suffix (_l or _r); empty past the last bone */
std::string HandBoneName(vr::BoneIndex_t bone, bool rightHand);
/** The inverse of BaseInput::ParentSpaceSkeletonToModelSpace */
void ModelSpaceSkeletonToParentSpace(vr::VRBoneTransform_t* joints);

/** Compressed bone data, OpenComposite's own format: little-endian { char magic[4] = "OCSK"; uint16_t version = 1;
 * uint8_t space; uint8_t range; uint32_t count = 31; } then the 31 transforms */
constexpr uint32_t kCompressedBonesSize = 12 + 31 * sizeof(vr::VRBoneTransform_t);
/** Writes the bones into out if outSize is big enough; the size needed (kCompressedBonesSize) */
uint32_t CompressBones(OOVR_EVRSkeletalTransformSpace space, vr::EVRSkeletalMotionRange range, const vr::VRBoneTransform_t* bones31, void* out, uint32_t outSize);
/** Reads bones back into out31, converted to the space wanted. *storedSpace (if given) is the space they were in. */
vr::EVRInputError DecompressBones(const void* in, uint32_t size, std::optional<OOVR_EVRSkeletalTransformSpace> wanted, vr::VRBoneTransform_t* out31,
    OOVR_EVRSkeletalTransformSpace* storedSpace = nullptr);
