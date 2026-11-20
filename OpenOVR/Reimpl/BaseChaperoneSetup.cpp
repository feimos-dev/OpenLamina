#include "stdafx.h"

#define BASE_IMPL
#include "BaseChaperoneSetup.h"
#include "BaseSystem.h"

#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/JsonFile.h"
#include "Misc/LibMonado.h"
#include "Misc/ProgramEvents.h"
#include "Misc/SharedStore.h"
#include "convert.h"
#include "generated/static_bases.gen.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <string>

using namespace vr;

// Room setup: a working copy that programs change and commit to the store's Chaperone document, which every
// OpenComposite program then reads. The OpenXR runtime's stage is the room until something is committed. The zero
// poses are kept and reported; with applyZeroPoses, on Monado, the standing one also moves the runtime's stage through
// libmonado (ApplyZeroPoses), as does a recentre the user asks for (RecentreStage), and elsewhere the runtime's own
// stay in use. Raw tracking space is the runtime's stage
// (floorSpace), so it moves with it, where SteamVR's raw space never moves. The live collision bounds and
// IVRChaperone's play area stay the runtime's: it draws the boundary the user sees.

namespace {

// Copies count items into an app's buffer: with no buffer, only the count; one too small, false with the count needed
template <typename T>
bool CopyArray(const std::vector<T>& items, T* buffer, uint32_t* count)
{
	if (!count)
		return false;
	uint32_t have = *count;
	*count = (uint32_t)items.size();
	if (!buffer)
		return true;
	if (have < items.size())
		return false;
	std::copy(items.begin(), items.end(), buffer);
	return true;
}

void PostChangedEvents()
{
	// For this program and every other one
	for (EVREventType type : { VREvent_ChaperoneDataHasChanged, VREvent_ChaperoneUniverseHasChanged })
		OcPostEvent(type, k_unTrackedDeviceIndexInvalid, VREvent_Data_t{}, true);
}

// The same pose, to what a pose loses on its way through libmonado (a quaternion of floats)
bool Near(const glm::mat4& a, const glm::mat4& b)
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			if (!(std::fabs(a[c][r] - b[c][r]) < 1e-4f))
				return false;
	return true;
}

// The Offsets document's record of the stage: the zero pose applied, and the absolute offset that set
void RecordStage(Json::Value& offsets, const vr::HmdMatrix34_t& zero, const vr::HmdMatrix34_t& offset)
{
	Json::Value stage(Json::objectValue);
	stage["zero"] = ChaperoneData::MatrixToJson(zero);
	stage["offset"] = ChaperoneData::MatrixToJson(offset);
	offsets["stage"] = stage;
}

// The width and depth of the OpenXR runtime's play area; false if it has none (or no session yet)
bool RuntimeStageSize(float& width, float& depth)
{
	BackendManager* backends = BackendManager::InstancePtr();
	int count = 0;
	if (!backends || !backends->GetPlayAreaPoints(nullptr, &count) || count < 2)
		return false;
	std::vector<HmdVector3_t> points(count);
	if (!backends->GetPlayAreaPoints(points.data(), nullptr))
		return false;
	float minX = points[0].v[0], maxX = minX, minZ = points[0].v[2], maxZ = minZ;
	for (const HmdVector3_t& p : points) {
		minX = std::min(minX, p.v[0]);
		maxX = std::max(maxX, p.v[0]);
		minZ = std::min(minZ, p.v[2]);
		maxZ = std::max(maxZ, p.v[2]);
	}
	width = maxX - minX;
	depth = maxZ - minZ;
	return true;
}

} // namespace

std::optional<vr::HmdMatrix34_t> BaseChaperoneSetup::ApplyZeroPoses(Json::Value& offsets, const ChaperoneData& committed, LibMonado& monado)
{
	if (!monado.Available() || !offsets.isObject())
		return std::nullopt;
	const glm::mat4 zero = S2G_m34(committed.standingZero);
	const Json::Value record = offsets.get("stage", Json::Value());
	std::optional<vr::HmdMatrix34_t> now = monado.GetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage);

	// Without libmonado's get call: the zero pose as the absolute offset
	glm::mat4 target = zero;
	if (now) {
		// The runtime's own offset: the one now, less the zero pose applied if the offset is still the one that set
		glm::mat4 base = S2G_m34(*now);
		if (record.isObject() && record.isMember("zero") && record.isMember("offset")) {
			const glm::mat4 set = S2G_m34(ChaperoneData::MatrixFromJson(record["offset"]));
			if (Near(base, set))
				base = set * glm::affineInverse(S2G_m34(ChaperoneData::MatrixFromJson(record["zero"])));
		}
		target = base * zero;
		if (Near(target, S2G_m34(*now))) {
			// There already: recorded as applied, unless nothing was ever applied and nothing is
			if (record.isObject() || !Near(zero, glm::mat4(1.0f)))
				RecordStage(offsets, committed.standingZero, *now);
			return std::nullopt;
		}
	}
	// A refusal is logged once, and tried again on the next commit
	if (!monado.SetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage, G2S_m34(target)))
		return std::nullopt;
	RecordStage(offsets, committed.standingZero, G2S_m34(target));
	return now;
}

vr::HmdMatrix34_t BaseChaperoneSetup::RecentredOrigin(const vr::HmdMatrix34_t& headInStage)
{
	const glm::mat4 head = S2G_m34(headInStage);
	// The way the head faces, along the floor; looking straight up or down, the way its top or bottom points
	glm::vec3 forward = -glm::vec3(head[2]);
	const bool down = forward.y < 0;
	forward.y = 0;
	if (glm::length(forward) < 1e-3f) {
		forward = glm::vec3(head[1]) * (down ? 1.0f : -1.0f);
		forward.y = 0;
	}
	const float yaw = glm::length(forward) < 1e-3f ? 0.0f : std::atan2(-forward.x, -forward.z);
	glm::mat4 origin = glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
	origin[3] = glm::vec4(head[3].x, 0.0f, head[3].z, 1.0f);
	return G2S_m34(origin);
}

std::optional<vr::HmdMatrix34_t> BaseChaperoneSetup::MoveStage(Json::Value& offsets, const vr::HmdMatrix34_t& origin, LibMonado& monado)
{
	std::optional<vr::HmdMatrix34_t> now = monado.GetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage);
	if (!now)
		return std::nullopt;
	const vr::HmdMatrix34_t target = G2S_m34(S2G_m34(*now) * S2G_m34(origin));
	if (!monado.SetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage, target))
		return std::nullopt;
	const Json::Value record = offsets.isObject() ? offsets.get("stage", Json::Value()) : Json::Value();
	if (record.isObject() && record.isMember("zero") && record.isMember("offset")
	    && Near(S2G_m34(*now), S2G_m34(ChaperoneData::MatrixFromJson(record["offset"]))))
		offsets["stage"]["offset"] = ChaperoneData::MatrixToJson(target);
	return now;
}

bool BaseChaperoneSetup::RecentreStage(const vr::HmdMatrix34_t& headInStage)
{
	LibMonado& monado = LibMonado::Get();
	if (!oovr_global_configuration.ApplyZeroPoses() || !monado.Available())
		return false;
	// Read, moved and recorded under the store's lock, as a committed zero pose is
	const vr::HmdMatrix34_t origin = RecentredOrigin(headInStage);
	std::optional<vr::HmdMatrix34_t> before;
	const bool recorded = SharedStore::Get().Update(StoreDoc::Offsets, [&](Json::Value& offsets) { before = MoveStage(offsets, origin, monado); });
	if (!before)
		return false;
	if (!recorded) {
		// Moved but not recorded: put back, so that the next commit doesn't apply its zero pose on top
		monado.SetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage, *before);
		return false;
	}
	PostChangedEvents();
	return true;
}

ChaperoneData BaseChaperoneSetup::LiveData(bool* found)
{
	if (found)
		*found = true;
	if (auto committed = ChaperoneData::FromJson(SharedStore::Get().Read(StoreDoc::Chaperone)))
		return *committed;

	// The runtime's stage
	float width, depth;
	if (RuntimeStageSize(width, depth))
		return ChaperoneData::FromStage(width, depth);
	if (found)
		*found = false;
	return ChaperoneData{};
}

ChaperoneData& BaseChaperoneSetup::Working(bool editing)
{
	// Neither a committed room nor the runtime's stage (before tracking starts, say): looked for again next time
	if (!working || !workingKept)
		working = LiveData(&workingKept);
	if (editing)
		workingKept = true;
	return *working;
}

bool BaseChaperoneSetup::CommitWorkingCopy(EChaperoneConfigFile configFile)
{
	std::lock_guard guard(lock);
	// The temporary config is a preview: nothing to keep
	if (configFile == EChaperoneConfigFile_Temp)
		return true;
	Json::Value data = Working().ToJson();
	if (!SharedStore::Get().Update(StoreDoc::Chaperone, [&](Json::Value& doc) { doc = data; }))
		return false;
	// The standing zero pose onto Monado's stage: read, applied and recorded under the store's lock
	LibMonado& monado = LibMonado::Get();
	if (oovr_global_configuration.ApplyZeroPoses() && monado.Available()) {
		const ChaperoneData committed = Working();
		std::optional<vr::HmdMatrix34_t> before;
		bool recorded = SharedStore::Get().Update(StoreDoc::Offsets, [&](Json::Value& offsets) { before = ApplyZeroPoses(offsets, committed, monado); });
		// Moved but not recorded: put back, so that the next commit doesn't apply it on top
		if (!recorded && before)
			monado.SetReferenceSpaceOffset(LibMonado::kReferenceSpaceStage, *before);
	}
	PostChangedEvents();
	return true;
}
void BaseChaperoneSetup::RevertWorkingCopy()
{
	std::lock_guard guard(lock);
	working = LiveData(&workingKept);
}
bool BaseChaperoneSetup::GetWorkingPlayAreaSize(float* pSizeX, float* pSizeZ)
{
	std::lock_guard guard(lock);
	if (!pSizeX || !pSizeZ)
		return false;
	*pSizeX = Working().playArea[0];
	*pSizeZ = Working().playArea[1];
	return *pSizeX > 0 && *pSizeZ > 0;
}
bool BaseChaperoneSetup::GetWorkingPlayAreaRect(HmdQuad_t* rect)
{
	std::lock_guard guard(lock);
	if (!rect)
		return false;
	const ChaperoneData& d = Working();
	*rect = ChaperoneData::FloorQuad(d.playArea[0], d.playArea[1]);
	return d.playArea[0] > 0 && d.playArea[1] > 0;
}
bool BaseChaperoneSetup::GetWorkingCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount)
{
	std::lock_guard guard(lock);
	return CopyArray(Working().collisionBounds, pQuadsBuffer, punQuadsCount);
}
bool BaseChaperoneSetup::GetLiveCollisionBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount)
{
	// The runtime's boundary, as a wall on each edge of the play area; none if it has no play area
	float width, depth;
	if (!RuntimeStageSize(width, depth)) {
		if (punQuadsCount)
			*punQuadsCount = 0;
		return false;
	}
	return CopyArray(ChaperoneData::WallQuads(width, depth, ChaperoneData::kWallHeight), pQuadsBuffer, punQuadsCount);
}
bool BaseChaperoneSetup::GetWorkingSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t* pmatSeatedZeroPoseToRawTrackingPose)
{
	std::lock_guard guard(lock);
	if (!pmatSeatedZeroPoseToRawTrackingPose)
		return false;
	*pmatSeatedZeroPoseToRawTrackingPose = Working().seatedZero;
	return true;
}
bool BaseChaperoneSetup::GetWorkingStandingZeroPoseToRawTrackingPose(HmdMatrix34_t* pmatStandingZeroPoseToRawTrackingPose)
{
	std::lock_guard guard(lock);
	if (!pmatStandingZeroPoseToRawTrackingPose)
		return false;
	*pmatStandingZeroPoseToRawTrackingPose = Working().standingZero;
	return true;
}
void BaseChaperoneSetup::SetWorkingPlayAreaSize(float sizeX, float sizeZ)
{
	// VRGIN (a VR mod framework) sets it to hide the chaperone during seated play: kept in the working copy only,
	// unless the program commits it
	std::lock_guard guard(lock);
	Working(true).playArea[0] = sizeX;
	Working(true).playArea[1] = sizeZ;
}
void BaseChaperoneSetup::SetWorkingCollisionBoundsInfo(VR_ARRAY_COUNT(unQuadsCount) HmdQuad_t* pQuadsBuffer, uint32_t unQuadsCount)
{
	std::lock_guard guard(lock);
	if (!pQuadsBuffer)
		unQuadsCount = 0;
	Working(true).collisionBounds.assign(pQuadsBuffer, pQuadsBuffer + unQuadsCount);
}
void BaseChaperoneSetup::SetWorkingSeatedZeroPoseToRawTrackingPose(const HmdMatrix34_t* pMatSeatedZeroPoseToRawTrackingPose)
{
	std::lock_guard guard(lock);
	if (pMatSeatedZeroPoseToRawTrackingPose)
		Working(true).seatedZero = *pMatSeatedZeroPoseToRawTrackingPose;
}
void BaseChaperoneSetup::SetWorkingStandingZeroPoseToRawTrackingPose(const HmdMatrix34_t* pMatStandingZeroPoseToRawTrackingPose)
{
	std::lock_guard guard(lock);
	if (pMatStandingZeroPoseToRawTrackingPose)
		Working(true).standingZero = *pMatStandingZeroPoseToRawTrackingPose;
}
void BaseChaperoneSetup::ReloadFromDisk(EChaperoneConfigFile configFile)
{
	std::lock_guard guard(lock);
	working = LiveData(&workingKept);
}
bool BaseChaperoneSetup::GetLiveSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t* pmatSeatedZeroPoseToRawTrackingPose)
{
	if (!pmatSeatedZeroPoseToRawTrackingPose)
		return false;
	// A committed one; without, none
	if (auto committed = ChaperoneData::FromJson(SharedStore::Get().Read(StoreDoc::Chaperone))) {
		*pmatSeatedZeroPoseToRawTrackingPose = committed->seatedZero;
		return true;
	}
	*pmatSeatedZeroPoseToRawTrackingPose = {};
	return false;
}
void BaseChaperoneSetup::SetWorkingCollisionBoundsTagsInfo(VR_ARRAY_COUNT(unTagCount) uint8_t* pTagsBuffer, uint32_t unTagCount)
{
	std::lock_guard guard(lock);
	if (!pTagsBuffer)
		unTagCount = 0;
	Working(true).tags.assign(pTagsBuffer, pTagsBuffer + unTagCount);
}
bool BaseChaperoneSetup::GetLiveCollisionBoundsTagsInfo(VR_OUT_ARRAY_COUNT(punTagCount) uint8_t* pTagsBuffer, uint32_t* punTagCount)
{
	std::vector<uint8_t> tags;
	if (auto committed = ChaperoneData::FromJson(SharedStore::Get().Read(StoreDoc::Chaperone)))
		tags = committed->tags;
	if (tags.empty()) {
		if (punTagCount)
			*punTagCount = 0;
		return false;
	}
	return CopyArray(tags, pTagsBuffer, punTagCount);
}
bool BaseChaperoneSetup::SetWorkingPhysicalBoundsInfo(VR_ARRAY_COUNT(unQuadsCount) HmdQuad_t* pQuadsBuffer, uint32_t unQuadsCount)
{
	std::lock_guard guard(lock);
	if (!pQuadsBuffer)
		unQuadsCount = 0;
	Working(true).physicalBounds.assign(pQuadsBuffer, pQuadsBuffer + unQuadsCount);
	// Kept, but answered as SteamVR answers it for any quads
	return false;
}
bool BaseChaperoneSetup::GetLivePhysicalBoundsInfo(VR_OUT_ARRAY_COUNT(punQuadsCount) HmdQuad_t* pQuadsBuffer, uint32_t* punQuadsCount)
{
	std::vector<HmdQuad_t> quads;
	if (auto committed = ChaperoneData::FromJson(SharedStore::Get().Read(StoreDoc::Chaperone)))
		quads = committed->physicalBounds;
	if (quads.empty()) {
		if (punQuadsCount)
			*punQuadsCount = 0;
		return false;
	}
	return CopyArray(quads, pQuadsBuffer, punQuadsCount);
}
bool BaseChaperoneSetup::ExportLiveToBuffer(VR_OUT_STRING() char* pBuffer, uint32_t* pnBufferLength)
{
	if (!pnBufferLength)
		return false;
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	std::string text = Json::writeString(builder, LiveData().ToJson());
	uint32_t needed = (uint32_t)text.size() + 1;
	uint32_t have = *pnBufferLength;
	*pnBufferLength = needed;
	if (!pBuffer || have < needed) {
		if (pBuffer && have)
			pBuffer[0] = 0;
		return false;
	}
	memcpy(pBuffer, text.c_str(), needed);
	return true;
}
bool BaseChaperoneSetup::ImportFromBufferToWorking(const char* pBuffer, uint32_t nImportFlags)
{
	if (!pBuffer)
		return false;
	Json::Value value;
	std::string error;
	if (!ParseJsonLenient(pBuffer, value, error))
		return false;
	std::optional<ChaperoneData> imported = ChaperoneData::FromJson(value);
	if (!imported)
		return false;
	std::lock_guard guard(lock);
	ChaperoneData& d = Working(true);
	if (nImportFlags & EChaperoneImport_BoundsOnly) {
		d.playArea[0] = imported->playArea[0];
		d.playArea[1] = imported->playArea[1];
		d.collisionBounds = imported->collisionBounds;
		d.tags = imported->tags;
	} else {
		d = *imported;
	}
	return true;
}
void BaseChaperoneSetup::SetWorkingPerimeter(VR_ARRAY_COUNT(unPointCount) const HmdVector2_t* pPointBuffer, uint32_t unPointCount)
{
	std::lock_guard guard(lock);
	if (!pPointBuffer)
		unPointCount = 0;
	Working(true).perimeter.assign(pPointBuffer, pPointBuffer + unPointCount);
}
void BaseChaperoneSetup::ShowWorkingSetPreview()
{
	// The working copy's play area, drawn as the bounds' walls (whole, whether or not drawBounds is on) until hidden
	previewShown = true;
}
void BaseChaperoneSetup::HideWorkingSetPreview()
{
	previewShown = false;
}
std::optional<std::pair<float, float>> BaseChaperoneSetup::PreviewedPlayArea()
{
	if (!previewShown)
		return std::nullopt;
	std::lock_guard preview(previewLock);
	std::unique_lock guard(lock, std::try_to_lock);
	if (guard.owns_lock())
		previewedArea = { Working().playArea[0], Working().playArea[1] };
	return previewedArea;
}
void BaseChaperoneSetup::RoomSetupStarting()
{
	// The OpenXR runtime has its own room setup
}

void BaseChaperoneSetup::SetWorkingWallTagInfo(VR_ARRAY_COUNT(unTagCount) uint8_t* pTagsBuffer, uint32_t unTagCount)
{
	SetWorkingCollisionBoundsTagsInfo(pTagsBuffer, unTagCount);
}
bool BaseChaperoneSetup::GetLiveWallTagInfo(VR_OUT_ARRAY_COUNT(punTagCount) uint8_t* pTagsBuffer, uint32_t* punTagCount)
{
	return GetLiveCollisionBoundsTagsInfo(pTagsBuffer, punTagCount);
}
