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

Compositor::~Compositor()
{
	if (chain) {
		OOVR_FAILED_XR_SOFT_ABORT(xrDestroySwapchain(chain));
		chain = XR_NULL_HANDLE;
	}
}

void Compositor::Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	bool invertInCompositor = oovr_global_configuration.InvertUsingShaders();
	if (bounds && bounds->vMin == 0.0f && bounds->vMax == 1.0f && bounds->uMin == 0.0f && bounds->uMax == 1.0f)
		bounds = nullptr;
	// After a failure the next tries come further apart (1, 3, 7, ... frames, at most 90), so a swapchain that can't be
	// made isn't remade (and logged) at every Submit; one that fails once is tried again at the next
	if (retryDelay > 0) {
		retryDelay--;
		lastResult = lastFailure;
		subImage.swapchain = XR_NULL_HANDLE;
		return;
	}
	lastResult = XR_SUCCESS;
	submitSlice = SubmitArraySlice::Current();
	CopyToSwapchain(texture, invertInCompositor ? bounds : nullptr, eye, submitFlags);
	if (XR_FAILED(lastResult)) {
		lastFailure = lastResult;
		failuresInARow = std::min(failuresInARow + 1, 8);
		retryDelay = std::min((1 << (failuresInARow - 1)) - 1, 90);
	} else {
		failuresInARow = 0;
	}
	// A failed swapchain call leaves this eye out of the frame (XrBackend checks the swapchain)
	subImage.swapchain = XR_FAILED(lastResult) ? XR_NULL_HANDLE : GetSwapChain();
	subImage.imageArrayIndex = 0; // This is *not* the swapchain index
	XrExtent2Di src = GetSrcSize();
	CalculateViewport(invertInCompositor ? nullptr : bounds, src.width, src.height, true, subImage.imageRect);
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

bool Compositor::CalculateViewport(const vr::VRTextureBounds_t* ptrBounds, int32_t width, int32_t height, bool supportsInvert, XrRect2Di& viewport)
{
	bool submitVerticallyFlipped = false;

	if (ptrBounds) {
		vr::VRTextureBounds_t newBounds = *ptrBounds;
		if (!supportsInvert && newBounds.vMin > newBounds.vMax) {
			float newMax = newBounds.vMin;
			newBounds.vMin = newBounds.vMax;
			newBounds.vMax = newMax;
			submitVerticallyFlipped = true;
		} else {
			submitVerticallyFlipped = false;
		}

		viewport.offset.x = (int)(newBounds.uMin * (float)width);
		viewport.offset.y = (int)(newBounds.vMin * (float)height);
		viewport.extent.width = (int)((newBounds.uMax - newBounds.uMin) * (float)width);
		viewport.extent.height = (int)((newBounds.vMax - newBounds.vMin) * (float)height);
	} else {
		viewport.offset.x = viewport.offset.y = 0;
		viewport.extent.width = width;
		viewport.extent.height = height;
		submitVerticallyFlipped = false;
	}
	return submitVerticallyFlipped;
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

bool Compositor::ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t*, Image&)
{
	OOVR_LOG_ONCEF("Textures of type %d can't be read back into memory", tex ? (int)tex->eType : -1);
	return false;
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
