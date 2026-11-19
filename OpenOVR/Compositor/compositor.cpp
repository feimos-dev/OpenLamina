#include "stdafx.h"

#include "../Misc/Config.h"
#include "../Misc/Image.h"
#include "compositor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

int RetryDelayAfterFailure(bool transient, bool sameTextureAsLastFailure, int& failuresInARow)
{
	if (transient)
		return 0;
	failuresInARow = sameTextureAsLastFailure ? (std::min)(failuresInARow + 1, 8) : 1;
	return (std::min)((1 << (failuresInARow - 1)) - 1, 90);
}

Compositor::~Compositor()
{
	if (chain) {
		OOVR_FAILED_XR_SOFT_ABORT(xrDestroySwapchain(chain));
		chain = XR_NULL_HANDLE;
	}
}

void Compositor::Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	if (bounds && bounds->vMin == 0.0f && bounds->vMax == 1.0f && bounds->uMin == 0.0f && bounds->uMax == 1.0f)
		bounds = nullptr;
	// After a failure the next tries come further apart (1, 3, 7, ... frames, at most 90), so a swapchain that can't be
	// made isn't remade (and logged) at every Submit; one that fails once is tried again at the next. Another texture
	// than the one that failed is tried at once.
	if (retryDelay > 0 && texture->handle != lastFailedHandle)
		retryDelay = 0;
	if (retryDelay > 0) {
		retryDelay--;
		lastResult = lastFailure;
		subImage.swapchain = XR_NULL_HANDLE;
		return;
	}
	lastResult = XR_SUCCESS;
	submitSlice = SubmitArraySlice::Current();
	copiedArea = {};
	transientFailure = false;
	CopyToSwapchain(texture, bounds, eye, submitFlags);
	if (XR_FAILED(lastResult)) {
		lastFailure = lastResult;
		retryDelay = RetryDelayAfterFailure(transientFailure, texture->handle == lastFailedHandle, failuresInARow);
		lastFailedHandle = texture->handle;
	} else {
		failuresInARow = 0;
	}
	// A failed swapchain call leaves this eye out of the frame (XrBackend checks the swapchain)
	subImage.swapchain = XR_FAILED(lastResult) ? XR_NULL_HANDLE : GetSwapChain();
	subImage.imageArrayIndex = 0; // This is *not* the swapchain index
	// The copy put the bounds' area at the image's origin (flipped bounds flipped back); a compositor that doesn't say
	// how much it copied filled the whole image
	subImage.imageRect.offset = { 0, 0 };
	subImage.imageRect.extent = copiedArea.width > 0 && copiedArea.height > 0 ? copiedArea : GetSrcSize();
}

// Each failing call (and result) is logged the first time only
static void LogSwapchainFailure(const char* what, XrResult res)
{
	static std::mutex lock;
	static std::set<std::string> logged;
	std::lock_guard<std::mutex> guard(lock);
	if (logged.insert(std::string(what) + "/" + std::to_string((int)res)).second)
		OOVR_LOGF("%s failed: %d (that eye is left out of the frame; logged once)", what, (int)res);
}

bool Compositor::SwapchainCallFailed(XrResult res, const char* what)
{
	if (XR_SUCCEEDED(res) && res != XR_TIMEOUT_EXPIRED)
		return false;
	lastResult = res == XR_TIMEOUT_EXPIRED ? XR_ERROR_RUNTIME_FAILURE : res;
	LogSwapchainFailure(what, res);
	return true;
}

bool Compositor::AcquireAndWait(uint32_t imageCount, uint32_t& index, int64_t timeout)
{
	if (!chain) {
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return false;
	}
	// An image acquired by an earlier call whose wait timed out is waited on again, not acquired anew
	if (!waitPending || pendingChain != chain) {
		XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t acquired = 0;
		if (SwapchainCallFailed(xrAcquireSwapchainImage(chain, &acquireInfo, &acquired), "xrAcquireSwapchainImage"))
			return false;
		pendingIndex = acquired;
		pendingChain = chain;
		waitPending = true;
	}

	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = timeout;
	XrResult res = xrWaitSwapchainImage(chain, &waitInfo);
	if (res == XR_TIMEOUT_EXPIRED) {
		// Still the compositor's: this frame's image is skipped, and the next Invoke waits for it again
		SwapchainCallFailed(res, "xrWaitSwapchainImage (timed out)");
		transientFailure = true;
		return false;
	}
	if (SwapchainCallFailed(res, "xrWaitSwapchainImage")) {
		// The image stays acquired, and can't be released unwaited: this swapchain goes, and the next Submit makes
		// a new one (the cleared createInfo matches no texture)
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
		createInfo = {};
		waitPending = false;
		return false;
	}
	waitPending = false;

	if (pendingIndex >= imageCount) {
		// The runtime gave an image the swapchain doesn't have: hand it back unwritten
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(chain, &releaseInfo);
		SwapchainCallFailed(XR_ERROR_RUNTIME_FAILURE, "xrAcquireSwapchainImage (index out of range)");
		return false;
	}
	index = pendingIndex;
	return true;
}

static thread_local std::optional<uint32_t> currentSubmitSlice;

SubmitArraySlice::SubmitArraySlice(uint32_t slice)
    : previous(currentSubmitSlice)
{
	currentSubmitSlice = slice;
}

SubmitArraySlice::~SubmitArraySlice()
{
	currentSubmitSlice = previous;
}

std::optional<uint32_t> SubmitArraySlice::Current()
{
	return currentSubmitSlice;
}

const std::vector<int64_t>& Compositor::RuntimeFormats()
{
	static const std::vector<int64_t> none;
	return xr_gbl ? xr_gbl->SwapchainFormats() : none;
}

bool Compositor::InvokeDepth(const vr::Texture_t* depth, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage)
{
	lastResult = XR_SUCCESS;
	subImage = {};
	if (!depth || !depth->handle)
		return false;
	// The colour is turned over in its copy; its depth would have to be too, which no copy here does
	const vr::VRTextureBounds_t b = Bounds(bounds);
	if (b.uMin > b.uMax || b.vMin > b.vMax)
		OC_UNSUPPORTED(false, "A depth texture submitted with flipped bounds: depth not sent");
	if (!CopyDepth(depth) || !chain || XR_FAILED(lastResult))
		return false;
	// The whole texture is in the image: the bounds' area of it is shown with the colour
	TextureRegion region;
	if (!RegionFromBounds(bounds, createInfo.width, createInfo.height, region))
		return false;
	subImage.swapchain = chain;
	subImage.imageRect.offset = { (int32_t)region.x0, (int32_t)region.y0 };
	subImage.imageRect.extent = { (int32_t)region.Width(), (int32_t)region.Height() };
	subImage.imageArrayIndex = 0;
	return true;
}

bool Compositor::CopyDepth(const vr::Texture_t* depth)
{
	OOVR_LOG_ONCEF("Depth textures of type %d can't be sent to the runtime", depth ? (int)depth->eType : -1);
	return false;
}

bool Compositor::MakeDepthChain(int64_t format, uint32_t width, uint32_t height)
{
	if (chain) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	ForgetAcquiredImage();
	createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	createInfo.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	createInfo.format = format;
	createInfo.sampleCount = 1;
	createInfo.width = width;
	createInfo.height = height;
	createInfo.faceCount = 1;
	createInfo.arraySize = 1;
	createInfo.mipCount = 1;
	if (SwapchainCallFailed(xrCreateSwapchain(xr_session.get(), &createInfo, &chain), "xrCreateSwapchain (depth)")) {
		chain = XR_NULL_HANDLE;
		createInfo = {};
		return false;
	}
	OOVR_LOGF("Depth swapchain: %ux%u in format %d", width, height, (int)format);
	return true;
}

bool Compositor::ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t*, Image&)
{
	OOVR_LOG_ONCEF("Textures of type %d can't be read back into memory", tex ? (int)tex->eType : -1);
	return false;
}

void EyeImageSources(const bool submitted[2], const bool usable[2], int source[2])
{
	for (int eye = 0; eye < 2; eye++) {
		const int other = 1 - eye;
		if (submitted[eye])
			source[eye] = usable[eye] ? eye : -1;
		else
			source[eye] = submitted[other] && usable[other] ? other : -1;
	}
}

bool RegionFromBounds(const vr::VRTextureBounds_t* bounds, uint32_t width, uint32_t height, TextureRegion& out)
{
	vr::VRTextureBounds_t b = bounds ? *bounds : vr::VRTextureBounds_t{ 0, 0, 1, 1 };
	out = {};
	out.flipX = b.uMin > b.uMax;
	out.flipY = b.vMin > b.vMax;
	float u0 = std::min(b.uMin, b.uMax), u1 = std::max(b.uMin, b.uMax);
	float v0 = std::min(b.vMin, b.vMax), v1 = std::max(b.vMin, b.vMax);
	// Clamped to the texture; NaN bounds give an empty area
	auto at = [](float f, uint32_t size) {
		if (!(f > 0))
			return 0u;
		if (f >= 1)
			return size;
		return (uint32_t)std::lround(f * (float)size);
	};
	out.x0 = at(u0, width), out.x1 = at(u1, width);
	out.y0 = at(v0, height), out.y1 = at(v1, height);
	// An axis with no area (bounds left all zero, as some mods send them) shows the texture's whole width or height
	if (out.x1 <= out.x0) {
		OOVR_LOG_ONCEF("Texture bounds with no width (u %f to %f): the texture's whole width is shown", (double)b.uMin, (double)b.uMax);
		out.x0 = 0, out.x1 = width, out.flipX = false;
	}
	if (out.y1 <= out.y0) {
		OOVR_LOG_ONCEF("Texture bounds with no height (v %f to %f): the texture's whole height is shown", (double)b.vMin, (double)b.vMax);
		out.y0 = 0, out.y1 = height, out.flipY = false;
	}
	return out.x1 > out.x0 && out.y1 > out.y0;
}

void FlipImage(Image& image, bool flipX, bool flipY)
{
	if (image.Empty() || (!flipX && !flipY))
		return;
	const uint32_t w = image.width, h = image.height;
	std::vector<uint8_t> flipped(image.rgba.size());
	for (uint32_t y = 0; y < h; y++) {
		uint32_t sy = flipY ? h - 1 - y : y;
		for (uint32_t x = 0; x < w; x++) {
			uint32_t sx = flipX ? w - 1 - x : x;
			std::memcpy(&flipped[((size_t)y * w + x) * 4], &image.rgba[((size_t)sy * w + sx) * 4], 4);
		}
	}
	image.rgba.swap(flipped);
}
