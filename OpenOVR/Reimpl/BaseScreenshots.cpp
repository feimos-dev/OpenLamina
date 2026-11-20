#include "stdafx.h"
#define BASE_IMPL
#include "BaseScreenshots.h"

#include "../Misc/ProgramEvents.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

using namespace vr;
using EVRScreenshotError = BaseScreenshots::EVRScreenshotError;

namespace {
// A screenshot asked for and not yet submitted
struct Request {
	ScreenshotHandle_t handle = k_unScreenshotHandleInvalid;
	EVRScreenshotType type = VRScreenshotType_None;
	std::string preview, vrFile;
};

std::mutex lock;
// The types the game takes screenshots of itself, once it has hooked screenshots
std::vector<EVRScreenshotType> hooked;
bool isHooked = false;
std::vector<Request> requests;
ScreenshotHandle_t lastHandle = k_unScreenshotHandleInvalid;
// The most requests kept: a game that never submits doesn't make the list grow
constexpr size_t kMaxRequests = 32;

Request* Find(ScreenshotHandle_t handle)
{
	for (Request& r : requests)
		if (r.handle == handle && handle != k_unScreenshotHandleInvalid)
			return &r;
	return nullptr;
}

std::filesystem::path PathOf(const std::string& utf8)
{
	return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// The destination name with the extension of the format written: .png
std::filesystem::path WithPng(const std::string& destination)
{
	std::filesystem::path path = PathOf(destination);
	path.replace_extension(".png");
	return path;
}

bool CopyInto(const char* source, const std::string& destination)
{
	if (!source || !*source || destination.empty())
		return true; // nothing asked for
	std::error_code ec;
	const std::filesystem::path to = WithPng(destination);
	if (to.has_parent_path())
		std::filesystem::create_directories(to.parent_path(), ec);
	std::filesystem::copy_file(PathOf(source), to, std::filesystem::copy_options::overwrite_existing, ec);
	return !ec;
}

void PostScreenshotEvent(EVREventType event, ScreenshotHandle_t handle, EVRScreenshotType type)
{
	VREvent_Data_t data{};
	data.screenshot.handle = handle;
	data.screenshot.type = type;
	OcPostEvent(event, k_unTrackedDeviceIndexInvalid, data, false);
}
} // namespace

EVRScreenshotError BaseScreenshots::RequestScreenshot(ScreenshotHandle_t* pOutScreenshotHandle, EVRScreenshotType type, const char* pchPreviewFilename, const char* pchVRFilename)
{
	if (pOutScreenshotHandle)
		*pOutScreenshotHandle = k_unScreenshotHandleInvalid;
	if (!pOutScreenshotHandle || !pchPreviewFilename || (type != VRScreenshotType_Mono && !pchVRFilename))
		return VRScreenshotError_RequestFailed;
	ScreenshotHandle_t handle;
	{
		std::lock_guard<std::mutex> guard(lock);
		// Only the game takes the shot, and only of the types it said it can
		if (!isHooked || std::find(hooked.begin(), hooked.end(), type) == hooked.end()) {
			OOVR_LOG_ONCE("A screenshot was requested of a game that hasn't hooked screenshots of that type: not taken");
			return VRScreenshotError_RequestFailed;
		}
		if (requests.size() >= kMaxRequests)
			requests.erase(requests.begin());
		handle = ++lastHandle;
		requests.push_back({ handle, type, pchPreviewFilename, pchVRFilename ? pchVRFilename : "" });
	}
	*pOutScreenshotHandle = handle;
	PostScreenshotEvent(VREvent_RequestScreenshot, handle, type);
	return VRScreenshotError_None;
}

EVRScreenshotError BaseScreenshots::HookScreenshot(VR_ARRAY_COUNT(numTypes) const EVRScreenshotType* pSupportedTypes, int numTypes)
{
	if (numTypes < 0 || (numTypes > 0 && !pSupportedTypes))
		return VRScreenshotError_RequestFailed;
	// There are only a handful of screenshot types: a longer list only repeats them, so no more than that is read
	constexpr int kMaxTypes = 8;
	std::lock_guard<std::mutex> guard(lock);
	hooked.assign(pSupportedTypes, pSupportedTypes + (numTypes < kMaxTypes ? numTypes : kMaxTypes));
	isHooked = true;
	return VRScreenshotError_None;
}

EVRScreenshotType BaseScreenshots::GetScreenshotPropertyType(ScreenshotHandle_t screenshotHandle, EVRScreenshotError* pError)
{
	std::lock_guard<std::mutex> guard(lock);
	const Request* r = Find(screenshotHandle);
	if (pError)
		*pError = r ? VRScreenshotError_None : VRScreenshotError_NotFound;
	return r ? r->type : VRScreenshotType_None;
}

uint32_t BaseScreenshots::GetScreenshotPropertyFilename(ScreenshotHandle_t screenshotHandle, EVRScreenshotPropertyFilenames filenameType, VR_OUT_STRING() char* pchFilename, uint32_t cchFilename, EVRScreenshotError* pError)
{
	if (pchFilename && cchFilename)
		pchFilename[0] = 0;
	std::lock_guard<std::mutex> guard(lock);
	const Request* r = Find(screenshotHandle);
	if (!r || (filenameType != VRScreenshotPropertyFilenames_Preview && filenameType != VRScreenshotPropertyFilenames_VR)) {
		if (pError)
			*pError = r ? VRScreenshotError_RequestFailed : VRScreenshotError_NotFound;
		return 0;
	}
	const std::string& name = filenameType == VRScreenshotPropertyFilenames_Preview ? r->preview : r->vrFile;
	const uint32_t needed = (uint32_t)name.size() + 1;
	if (pchFilename && cchFilename) {
		if (cchFilename < needed) {
			if (pError)
				*pError = VRScreenshotError_BufferTooSmall;
			return needed;
		}
		memcpy(pchFilename, name.c_str(), needed);
	}
	if (pError)
		*pError = VRScreenshotError_None;
	return needed;
}

EVRScreenshotError BaseScreenshots::UpdateScreenshotProgress(ScreenshotHandle_t screenshotHandle, float flProgress)
{
	// The progress is only shown by the dashboard, which this isn't
	return VRScreenshotError_None;
}

EVRScreenshotError BaseScreenshots::TakeStereoScreenshot(ScreenshotHandle_t* pOutScreenshotHandle, const char* pchPreviewFilename, const char* pchVRFilename)
{
	if (pOutScreenshotHandle)
		*pOutScreenshotHandle = k_unScreenshotHandleInvalid;
	OC_UNSUPPORTED(VRScreenshotError_RequestFailed, "TakeStereoScreenshot: the compositor keeps no image of the game's frames to write");
}

EVRScreenshotError BaseScreenshots::SubmitScreenshot(ScreenshotHandle_t screenshotHandle, EVRScreenshotType type, const char* pchSourcePreviewFilename, const char* pchSourceVRFilename)
{
	Request r;
	{
		std::lock_guard<std::mutex> guard(lock);
		const Request* found = Find(screenshotHandle);
		if (!found)
			return VRScreenshotError_None; // no request of ours: no destination for the files
		r = *found;
		requests.erase(requests.begin() + (found - requests.data()));
	}
	// The files go where the request asked for them
	const bool copied = CopyInto(pchSourcePreviewFilename, r.preview) && CopyInto(pchSourceVRFilename, r.vrFile);
	PostScreenshotEvent(copied ? VREvent_ScreenshotTaken : VREvent_ScreenshotFailed, r.handle, r.type);
	return copied ? VRScreenshotError_None : VRScreenshotError_RequestFailed;
}
