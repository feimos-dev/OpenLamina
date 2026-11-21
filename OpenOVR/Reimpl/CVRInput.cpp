#include "stdafx.h"
#define GENFILE
#include "BaseCommon.h"

GEN_INTERFACE("Input", "003")
GEN_INTERFACE("Input", "004")
GEN_INTERFACE("Input", "005")
GEN_INTERFACE("Input", "006")
GEN_INTERFACE("Input", "007")
// version 8 and 9 are skipped
GEN_INTERFACE("Input", "010")
GEN_INTERFACE("Input", "011")

#include "generated/GVRInput.gen.h"

// The game's size argument is the size of the header's struct: BaseInput's own must match it on every target
static_assert(sizeof(OOVR_InputSkeletalActionData_t) == sizeof(vr::IVRInput_011::InputSkeletalActionData_t));
static_assert(sizeof(OOVR_InputOriginInfo_t) == sizeof(vr::IVRInput_011::InputOriginInfo_t));
static_assert(sizeof(OOVR_VRActiveActionSet_t) == sizeof(vr::IVRInput_011::VRActiveActionSet_t));
static_assert(sizeof(OOVR_VRSkeletalSummaryData_t) == sizeof(vr::IVRInput_011::VRSkeletalSummaryData_t));
static_assert(sizeof(OOVR_InputBindingInfo_t) == sizeof(vr::IVRInput_011::InputBindingInfo_t));
static_assert(offsetof(OOVR_InputBindingInfo_t, rchInputSourceType) == sizeof(vr::IVRInput_007::InputBindingInfo_t));

// Prior to IVRInput_005 / OpenVR 1.1.3 the input layer had a slightly different format, so adapt that
vr::EVRInputError CVRInput_004::GetSkeletalActionData(vr::VRActionHandle_t action, vr::IVRInput_004::InputSkeletalActionData_t* pActionData, uint32_t unActionDataSize, vr::VRInputValueHandle_t ulRestrictToDevice)
{
	// A null or wrong-sized struct is an argument error, as the main method answers
	if (!pActionData || unActionDataSize != sizeof(vr::IVRInput_004::InputSkeletalActionData_t))
		return vr::VRInputError_InvalidParam;
	// Set the size to what the main method is expecting
	unActionDataSize = sizeof(OOVR_InputSkeletalActionData_t);

	// Load the required value (the main method's answer then says whether the action is usable)
	pActionData->boneCount = 0;
	base->GetBoneCount(action, &pActionData->boneCount);

	return base->GetSkeletalActionData(action, (OOVR_InputSkeletalActionData_t*)pActionData, unActionDataSize, ulRestrictToDevice);
}

// IVRInput_003 (SDK 1.0.15)'s VRActiveActionSet_t ends after ulSecondaryActionSet (24 bytes): today's adds a padding and a
// priority (32). Copied into today's form, priority 0, when the app passes the old size
static bool ToActiveSets003(const vr::IVRInput_003::VRActiveActionSet_t* pSets, uint32_t unSize, uint32_t unSetCount,
    std::vector<OOVR_VRActiveActionSet_t>& out)
{
	if (unSize != sizeof(vr::IVRInput_003::VRActiveActionSet_t) || (unSetCount && !pSets))
		return false;
	// A count no game's array has (a corrupt call) is an argument error, not an exception into the game
	try {
		out.assign(unSetCount, OOVR_VRActiveActionSet_t{});
	} catch (const std::exception&) { // bad_alloc, or length_error past max_size (on 32-bit)
		return false;
	}
	for (uint32_t i = 0; i < unSetCount; i++) {
		out[i].ulActionSet = pSets[i].ulActionSet;
		out[i].ulRestrictedToDevice = pSets[i].ulRestrictedToDevice;
		out[i].ulSecondaryActionSet = pSets[i].ulSecondaryActionSet;
	}
	return true;
}

vr::EVRInputError CVRInput_003::UpdateActionState(vr::IVRInput_003::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t, uint32_t unSetCount)
{
	std::vector<OOVR_VRActiveActionSet_t> sets;
	if (!ToActiveSets003(pSets, unSizeOfVRSelectedActionSet_t, unSetCount, sets))
		return vr::VRInputError_InvalidParam;
	return base->UpdateActionState(sets.data(), sizeof(OOVR_VRActiveActionSet_t), unSetCount);
}

vr::EVRInputError CVRInput_003::ShowBindingsForActionSet(vr::IVRInput_003::VRActiveActionSet_t* pSets, uint32_t unSizeOfVRSelectedActionSet_t,
    uint32_t unSetCount, vr::IVRInput_003::VRInputValueHandle_t originToHighlight)
{
	std::vector<OOVR_VRActiveActionSet_t> sets;
	if (!ToActiveSets003(pSets, unSizeOfVRSelectedActionSet_t, unSetCount, sets))
		return vr::VRInputError_InvalidParam;
	return base->ShowBindingsForActionSet(sets.data(), sizeof(OOVR_VRActiveActionSet_t), unSetCount, originToHighlight);
}
