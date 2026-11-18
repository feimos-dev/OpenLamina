#pragma once

#include "generated/interfaces/vrtypes.h"
#ifdef WIN32
// Windows template libraries
#include <atlbase.h>
#include <wrl/client.h>
#endif

#include "../Misc/xr_ext.h"
#include "../Misc/xrutil.h"

#include <memory>
#include <optional>
#include <vector>

#ifdef WIN32
using Microsoft::WRL::ComPtr;
#endif

typedef unsigned int GLuint;

struct Image;

// A texture's area from submitted bounds, in its own texel rows and columns (x0 < x1, y0 < y1), and whether the bounds
// flip it (uMin > uMax, vMin > vMax). False for an empty area.
struct TextureRegion {
	uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool flipX = false, flipY = false;
	uint32_t Width() const { return x1 - x0; }
	uint32_t Height() const { return y1 - y0; }
};
bool RegionFromBounds(const vr::VRTextureBounds_t* bounds, uint32_t width, uint32_t height, TextureRegion& out);
// The bounds, or the whole texture's for none
inline vr::VRTextureBounds_t Bounds(const vr::VRTextureBounds_t* bounds)
{
	return bounds ? *bounds : vr::VRTextureBounds_t{ 0, 0, 1, 1 };
}

// Mirrors an image left to right and/or top to bottom
void FlipImage(Image& image, bool flipX, bool flipY);

// SubmitWithArrayIndex's slice, for the Invoke calls made on this thread while it's in scope
class SubmitArraySlice {
public:
	explicit SubmitArraySlice(uint32_t slice);
	~SubmitArraySlice();
	static std::optional<uint32_t> Current();

private:
	std::optional<uint32_t> previous;
};

class Compositor {
public:
	virtual ~Compositor();

	void Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage, std::optional<XruEye> eye = std::nullopt, vr::EVRSubmitFlags submitFlags = vr::Submit_Default);

	virtual void InvokeCubemap(const vr::Texture_t* textures) = 0;
	virtual bool SupportsCubemap() { return false; }

	virtual XrSwapchain GetSwapChain() { return chain; };

	/**
	 * Reads the bounds' area of a texture of this compositor's API back into memory (MSAA resolved first, flipped
	 * bounds flipped), as RGBA8. False, logged once, where that isn't possible (an API or format without a readback).
	 */
	virtual bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out);

	/**
	 * For a compositor that carries depth: copies a depth texture of its API into an image of a depth swapchain of the
	 * texture's size, in the runtime's depth format of the same bits. True with subImage set to the bounds' area of it
	 * (the colour's bounds, as the depth lines up with the colour); false (logged once) where that can't be done, as
	 * for flipped bounds. A compositor used for this is used for nothing else.
	 */
	bool InvokeDepth(const vr::Texture_t* depth, const vr::VRTextureBounds_t* bounds, XrSwapchainSubImage& subImage);

	/**
	 * What the last Invoke ran into: XR_SUCCESS, or the OpenXR swapchain call that failed (that eye's image is then
	 * left out of the frame, and Submit answers an error). The next Invoke tries again.
	 */
	XrResult LastResult() const { return lastResult; }

	virtual XrExtent2Di GetSrcSize() { return { static_cast<int32_t>(createInfo.width), static_cast<int32_t>(createInfo.height) }; }
	/**
	 * Loads and unloads some context required for submitting textures to LibOVR. LoadSubmitContext is
	 *  called before calling either Invoke or ovr_CommitTextureSwapChain, and ResetSubmitContext after
	 *  calling both of them.
	 */
	virtual void LoadSubmitContext() {};
	virtual void ResetSubmitContext() {};

protected:
	/**
	 * Copies the bounds' area of the texture (nullptr: all of it) to the origin of an image of a swapchain of the
	 * texture's size, flipping it where the bounds run backwards (uMin > uMax, vMin > vMax), and sets copiedArea to the
	 * area's size. A change of the bounds alone never makes a new swapchain.
	 */
	virtual void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) = 0;
	// The size of what CopyToSwapchain wrote at the image's origin: the part of it shown. Left at 0 x 0, the whole
	// image is shown.
	XrExtent2Di copiedArea{};

	// For the compositors' OpenXR swapchain calls: true (with lastResult set, and the call logged once) if it failed
	bool SwapchainCallFailed(XrResult res, const char* what);
	XrResult lastResult = XR_SUCCESS;

	// The slice of an array texture to show (SubmitWithArrayIndex), for CopyToSwapchain; without one, D3D11's arrays
	// show the eye's slice and others their first
	std::optional<uint32_t> submitSlice;

	// What xrEnumerateSwapchainFormats listed for this session (asked once a session)
	static const std::vector<int64_t>& RuntimeFormats();

	// InvokeDepth's copy, in each API's compositor: true once the texture is in the swapchain image and released
	virtual bool CopyDepth(const vr::Texture_t* depth);
	// A new depth swapchain (chain, createInfo) of format, width x height: false, logged once, if it can't be made
	bool MakeDepthChain(int64_t format, uint32_t width, uint32_t height);

	/**
	 * Acquires an image of chain and waits for it: true with its index, else false with lastResult set. After a
	 * timed-out wait the same image is waited on at the next call, so an image is never left acquired. An index
	 * beyond imageCount is released unwritten. The default timeout is 500 ms.
	 */
	bool AcquireAndWait(uint32_t imageCount, uint32_t& index, int64_t timeout = 500000000);
	// For a new swapchain (the old one's pending image went with it)
	void ForgetAcquiredImage() { waitPending = false; }

private:
	bool waitPending = false;
	uint32_t pendingIndex = 0;
	XrSwapchain pendingChain = XR_NULL_HANDLE;
	// Invoke's back-off after failures
	int failuresInARow = 0;
	int retryDelay = 0;
	XrResult lastFailure = XR_SUCCESS;

protected:

	XrSwapchain chain = XR_NULL_HANDLE;

	// The request used to create the current swapchain. This can be used to check if the swapchain needs recreating.
	XrSwapchainCreateInfo createInfo{};

	// The format specified by the game when creating the swapchain. This is used for verifying the format hasn't changed, since
	// we do fiddle with it a bit to get the SRGB stuff done correctly.
	int64_t createInfoFormat;
};
