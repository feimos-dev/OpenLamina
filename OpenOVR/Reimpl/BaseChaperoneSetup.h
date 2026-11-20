#pragma once
#include "BaseCommon.h"

#include "Misc/ChaperoneData.h"

#include "json/json.h"

#include <mutex>
#include <optional>

class LibMonado;

enum OOVR_EChaperoneConfigFile {
	EChaperoneConfigFile_Live = 1, // The live chaperone config, used by most applications and games
	EChaperoneConfigFile_Temp = 2, // The temporary chaperone config, used to live-preview collision bounds in room setup
};

enum OOVR_EChaperoneImportFlags {
	EChaperoneImport_BoundsOnly = 0x0001,
};

class BaseChaperoneSetup {
public:
	typedef OOVR_EChaperoneConfigFile EChaperoneConfigFile;
	typedef OOVR_EChaperoneImportFlags EChaperoneConfigFlags;

	/** Saves the current working copy to disk */
	bool CommitWorkingCopy(EChaperoneConfigFile configFile);

	/** Moves the runtime's stage to a committed room setup's standing zero pose, through libmonado (applyZeroPoses).
	 * offsets is the store's Offsets document, {"stage": {"zero": Z, "offset": O}}: the zero pose applied last and the
	 * absolute offset that set. The stage goes to base * zero, where base is the runtime's own offset: the one now
	 * less Z while it is still O, else the one now (Monado restarted, or another program moved it). Without libmonado's
	 * get call the zero pose is set as the absolute offset. Returns the offset the stage had before, when it moved it
	 * and libmonado told it. Without libmonado nothing moves. */
	static std::optional<vr::HmdMatrix34_t> ApplyZeroPoses(Json::Value& offsets, const ChaperoneData& committed, LibMonado& monado);

	/** A recentred stage's origin, as a pose in the stage now: on the floor under the head, turned about the vertical
	 * to face the way the head faces. The floor's height and the up direction stay. headInStage is the head's pose
	 * in the stage. */
	static vr::HmdMatrix34_t RecentredOrigin(const vr::HmdMatrix34_t& headInStage);

	/** Moves the runtime's stage to a new origin (a pose in the stage now), through libmonado: the offset it has
	 * now, with the origin on top of it. A record in the Offsets document of the offset it has now follows it (its
	 * zero pose stays), so that a room setup committed later still replaces the zero pose applied, and one that
	 * commits the same zero pose again moves nothing. Returns the offset the stage had before, when it moved it;
	 * nothing moves without libmonado or its get call. */
	static std::optional<vr::HmdMatrix34_t> MoveStage(Json::Value& offsets, const vr::HmdMatrix34_t& origin, LibMonado& monado);

	/** A recentre asked for by the user: on Monado with libmonado and applyZeroPoses, the stage moves to
	 * RecentredOrigin for every program, and every OpenLamina program is told as after a committed room setup.
	 * False if the stage didn't move. */
	static bool RecentreStage(const vr::HmdMatrix34_t& headInStage);

	/** Reverts the working copy to match the live chaperone calibration.
	 * To modify existing data this MUST be do WHILE getting a non-error ChaperoneCalibrationStatus.
	 * Only after this should you do gets and sets on the existing data. */
	void RevertWorkingCopy();

	/** Returns the width and depth of the Play Area (formerly named Soft Bounds) in X and Z from the working copy.
	 * Tracking space center (0,0,0) is the center of the Play Area. */
	bool GetWorkingPlayAreaSize(float* pSizeX, float* pSizeZ);

	/** Returns the 4 corner positions of the Play Area (formerly named Soft Bounds) from the working copy.
	 * Corners are in clockwise order.
	 * Tracking space center (0,0,0) is the center of the Play Area.
	 * It's a rectangle.
	 * 2 sides are parallel to the X axis and 2 sides are parallel to the Z axis.
	 * Height of every corner is 0Y (on the floor). **/
	bool GetWorkingPlayAreaRect(vr::HmdQuad_t* rect);

	/** Returns the number of Quads if the buffer points to null. Otherwise it returns Quads
	 * into the buffer up to the max specified from the working copy. */
	bool GetWorkingCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) vr::HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount);

	/** Returns the number of Quads if the buffer points to null. Otherwise it returns Quads
	 * into the buffer up to the max specified. */
	bool GetLiveCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) vr::HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount);

	/** Returns the preferred seated position from the working copy. */
	bool GetWorkingSeatedZeroPoseToRawTrackingPose(vr::HmdMatrix34_t* pmatSeatedZeroPoseToRawTrackingPose);

	/** Returns the standing origin from the working copy. */
	bool GetWorkingStandingZeroPoseToRawTrackingPose(vr::HmdMatrix34_t* pmatStandingZeroPoseToRawTrackingPose);

	/** Sets the Play Area in the working copy. */
	void SetWorkingPlayAreaSize(float sizeX, float sizeZ);

	/** Sets the Collision Bounds in the working copy. */
	void SetWorkingCollisionBoundsInfo(VR_ARRAY_COUNT(unQuadsCount) vr::HmdQuad_t* pQuadsBuffer, uint32_t unQuadsCount);

	/** Sets the preferred seated position in the working copy. */
	void SetWorkingSeatedZeroPoseToRawTrackingPose(const vr::HmdMatrix34_t* pMatSeatedZeroPoseToRawTrackingPose);

	/** Sets the preferred standing position in the working copy. */
	void SetWorkingStandingZeroPoseToRawTrackingPose(const vr::HmdMatrix34_t* pMatStandingZeroPoseToRawTrackingPose);

	/** Tear everything down and reload it from the file on disk */
	void ReloadFromDisk(EChaperoneConfigFile configFile);

	/** Returns the preferred seated position. */
	bool GetLiveSeatedZeroPoseToRawTrackingPose(vr::HmdMatrix34_t* pmatSeatedZeroPoseToRawTrackingPose);

	void SetWorkingCollisionBoundsTagsInfo(VR_ARRAY_COUNT(unTagCount) uint8_t* pTagsBuffer, uint32_t unTagCount);
	bool GetLiveCollisionBoundsTagsInfo(VR_OUT_ARRAY_COUNT(punTagCount) uint8_t* pTagsBuffer, uint32_t* punTagCount);

	// 0.9.14 and 0.9.15's IVRChaperoneSetup_004 (Valve added these to it without a new version, then renamed them
	// *CollisionBoundsTagsInfo in 005)
	void SetWorkingWallTagInfo(VR_ARRAY_COUNT(unTagCount) uint8_t* pTagsBuffer, uint32_t unTagCount);
	bool GetLiveWallTagInfo(VR_OUT_ARRAY_COUNT(punTagCount) uint8_t* pTagsBuffer, uint32_t* punTagCount);

	bool SetWorkingPhysicalBoundsInfo(VR_ARRAY_COUNT(unQuadsCount) vr::HmdQuad_t* pQuadsBuffer, uint32_t unQuadsCount);
	bool GetLivePhysicalBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) vr::HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount);

	bool ExportLiveToBuffer(VR_OUT_STRING() char* pBuffer, uint32_t* pnBufferLength);
	bool ImportFromBufferToWorking(const char* pBuffer, uint32_t nImportFlags);

	/** Sets the Collision Bounds in the working copy. */
	void SetWorkingPerimeter(VR_ARRAY_COUNT(unPointCount) const vr::HmdVector2_t* pPointBuffer, uint32_t unPointCount);

	/** Shows the chaperone data in the working set to preview in the compositor.*/
	void ShowWorkingSetPreview();

	/** Hides the chaperone data in the working set to preview in the compositor (if it was visible).*/
	void HideWorkingSetPreview();

	/** Fire an event that the tracking system can use to know room setup is about to begin. This lets the tracking
	 * system make any last minute adjustments that should be incorporated into the new setup.  If the user is adjusting
	 * live in HMD using a tweak tool, keep in mind that calling this might cause the user to see the room jump. */
	void RoomSetupStarting();

	/** The committed room setup: the store's Chaperone document, else the OpenXR runtime's stage. found, if given,
	 * says whether either existed. */
	static ChaperoneData LiveData(bool* found = nullptr);

	/** The working copy's play area while ShowWorkingSetPreview has it shown (BaseOverlay draws its walls), else
	 * nothing */
	std::optional<std::pair<float, float>> PreviewedPlayArea();

private:
	// Loaded from LiveData on first use, and by RevertWorkingCopy and ReloadFromDisk; lock held. While LiveData
	// found neither source, it's loaded again on each use, until editing (a Set*) keeps it.
	ChaperoneData& Working(bool editing = false);

	std::mutex lock;
	std::optional<ChaperoneData> working;
	bool workingKept = false;
	bool previewShown = false; // lock held
};
