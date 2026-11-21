#include "stdafx.h"
#define BASE_IMPL
#include "BaseScreenshots.h"

#include "../Misc/Image.h"
#include "../Misc/ProgramEvents.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
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
// Screenshots submitted without a request of ours (a game's own, with a handle of its own): kept, as SteamVR keeps
// them, so their properties are the submitted type and files
std::vector<Request> submitted;
ScreenshotHandle_t lastHandle = k_unScreenshotHandleInvalid;
// The most requests (and submitted screenshots) kept: a game that never submits doesn't make the list grow
constexpr size_t kMaxRequests = 32;

Request* Find(ScreenshotHandle_t handle)
{
	for (Request& r : requests)
		if (r.handle == handle && handle != k_unScreenshotHandleInvalid)
			return &r;
	return nullptr;
}

// A request, or else a screenshot submitted without one
const Request* FindAny(ScreenshotHandle_t handle)
{
	if (const Request* r = Find(handle))
		return r;
	for (const Request& r : submitted)
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

// The compositor's own screenshot, waiting for the game's eye images; one at a time
struct Capture {
	Request request;
	Image eyes[2];
};
std::mutex captureLock;
std::optional<Capture> capture;
std::atomic<bool> captureWanted{ false };

// A screenshot of the game's next frame, to be written to the request's files: false if one is already waiting
bool StartCapture(const Request& request)
{
	std::lock_guard<std::mutex> guard(captureLock);
	if (capture)
		return false;
	capture = Capture{ request, {} };
	captureWanted = true;
	return true;
}

// The compositor takes these types itself
bool CompositorTakes(EVRScreenshotType type)
{
	return type == VRScreenshotType_Mono || type == VRScreenshotType_Stereo;
}
} // namespace

bool BaseScreenshots::_WantsEyeImages()
{
	return captureWanted;
}

void BaseScreenshots::_FailEyeImages()
{
	Request failed;
	{
		std::lock_guard<std::mutex> guard(captureLock);
		if (!capture)
			return;
		failed = capture->request;
		capture.reset();
		captureWanted = false;
	}
	OOVR_LOG("The screenshot couldn't be taken: the game's eye image couldn't be read back");
	PostScreenshotEvent(VREvent_ScreenshotFailed, failed.handle, failed.type);
}

void BaseScreenshots::_OfferEyeImage(EVREye eye, Image image)
{
	if ((eye != Eye_Left && eye != Eye_Right) || image.Empty())
		return;
	Capture done;
	{
		std::lock_guard<std::mutex> guard(captureLock);
		if (!capture)
			return;
		// A frame's eyes go together: from its left eye on (a right eye before any left one is another frame's)
		if (eye == Eye_Right && capture->eyes[Eye_Left].Empty())
			return;
		capture->eyes[eye] = std::move(image);
		// A mono shot needs the left eye; a stereo one both
		const bool stereo = capture->request.type == VRScreenshotType_Stereo;
		if (capture->eyes[Eye_Left].Empty() || (stereo && capture->eyes[Eye_Right].Empty()))
			return;
		done = std::move(*capture);
		capture.reset();
		captureWanted = false;
	}

	// Written on the thread that submitted: a screenshot is worth a frame
	const Request& r = done.request;
	std::string why;
	bool written = WritePngFile(done.eyes[Eye_Left], WithPng(r.preview), &why);
	if (written && !r.vrFile.empty()) {
		const Image vr = r.type == VRScreenshotType_Stereo ? SideBySide(done.eyes[Eye_Left], done.eyes[Eye_Right]) : done.eyes[Eye_Left];
		written = WritePngFile(vr, WithPng(r.vrFile), &why);
	}
	if (!written)
		OOVR_LOGF("The screenshot couldn't be written: %s", why.c_str());
	PostScreenshotEvent(written ? VREvent_ScreenshotTaken : VREvent_ScreenshotFailed, r.handle, r.type);
}

EVRScreenshotError BaseScreenshots::RequestScreenshot(ScreenshotHandle_t* pOutScreenshotHandle, EVRScreenshotType type, const char* pchPreviewFilename, const char* pchVRFilename)
{
	if (pOutScreenshotHandle)
		*pOutScreenshotHandle = k_unScreenshotHandleInvalid;
	if (!pOutScreenshotHandle || !pchPreviewFilename || (type != VRScreenshotType_Mono && !pchVRFilename))
		return VRScreenshotError_RequestFailed;
	ScreenshotHandle_t handle;
	{
		std::lock_guard<std::mutex> guard(lock);
		// The game takes the shot of the types it said it can; the compositor takes a mono or stereo one of a game that
		// didn't say so, from its next frame
		const bool gameTakes = isHooked && std::find(hooked.begin(), hooked.end(), type) != hooked.end();
		if (!gameTakes) {
			if (!CompositorTakes(type)) {
				OOVR_LOG_ONCE("A screenshot was requested of a game that hasn't hooked screenshots of that type: not taken");
				return VRScreenshotError_RequestFailed;
			}
			const Request request{ lastHandle + 1, type, pchPreviewFilename, pchVRFilename ? pchVRFilename : "" };
			if (!StartCapture(request))
				return VRScreenshotError_RequestFailed; // another is waiting for a frame
			*pOutScreenshotHandle = ++lastHandle;
			return VRScreenshotError_None;
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
	const Request* r = FindAny(screenshotHandle);
	if (pError)
		*pError = r ? VRScreenshotError_None : VRScreenshotError_NotFound;
	return r ? r->type : VRScreenshotType_None;
}

uint32_t BaseScreenshots::GetScreenshotPropertyFilename(ScreenshotHandle_t screenshotHandle, EVRScreenshotPropertyFilenames filenameType, VR_OUT_STRING() char* pchFilename, uint32_t cchFilename, EVRScreenshotError* pError)
{
	if (pchFilename && cchFilename)
		pchFilename[0] = 0;
	std::lock_guard<std::mutex> guard(lock);
	const Request* r = FindAny(screenshotHandle);
	if (!r || (filenameType != VRScreenshotPropertyFilenames_Preview && filenameType != VRScreenshotPropertyFilenames_VR)) {
		if (pError)
			*pError = r ? VRScreenshotError_RequestFailed : VRScreenshotError_NotFound;
		return 0;
	}
	const std::string& name = filenameType == VRScreenshotPropertyFilenames_Preview ? r->preview : r->vrFile;
	const uint32_t needed = (uint32_t)name.size() + 1;
	if (pchFilename) {
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
	if (!pOutScreenshotHandle || !pchPreviewFilename || !*pchPreviewFilename || !pchVRFilename || !*pchVRFilename)
		return VRScreenshotError_RequestFailed;
	// The game's next frame, written by the compositor without asking the game
	std::lock_guard<std::mutex> guard(lock);
	const Request request{ lastHandle + 1, VRScreenshotType_Stereo, pchPreviewFilename, pchVRFilename };
	if (!StartCapture(request))
		return VRScreenshotError_RequestFailed; // another is waiting for a frame
	*pOutScreenshotHandle = ++lastHandle;
	return VRScreenshotError_None;
}

EVRScreenshotError BaseScreenshots::SubmitScreenshot(ScreenshotHandle_t screenshotHandle, EVRScreenshotType type, const char* pchSourcePreviewFilename, const char* pchSourceVRFilename)
{
	Request r;
	{
		std::lock_guard<std::mutex> guard(lock);
		const Request* found = Find(screenshotHandle);
		if (!found) {
			// No request of ours: no destination for the files, but the screenshot (of a type there is) is known from
			// now on
			if (screenshotHandle != k_unScreenshotHandleInvalid && type >= VRScreenshotType_None && type <= VRScreenshotType_StereoPanorama) {
				submitted.erase(std::remove_if(submitted.begin(), submitted.end(), [&](const Request& s) { return s.handle == screenshotHandle; }),
				    submitted.end());
				if (submitted.size() >= kMaxRequests)
					submitted.erase(submitted.begin());
				submitted.push_back({ screenshotHandle, type, pchSourcePreviewFilename ? pchSourcePreviewFilename : "",
				    pchSourceVRFilename ? pchSourceVRFilename : "" });
			}
			return VRScreenshotError_None;
		}
		r = *found;
		requests.erase(requests.begin() + (found - requests.data()));
	}
	// The files go where the request asked for them
	const bool copied = CopyInto(pchSourcePreviewFilename, r.preview) && CopyInto(pchSourceVRFilename, r.vrFile);
	PostScreenshotEvent(copied ? VREvent_ScreenshotTaken : VREvent_ScreenshotFailed, r.handle, r.type);
	return copied ? VRScreenshotError_None : VRScreenshotError_RequestFailed;
}
