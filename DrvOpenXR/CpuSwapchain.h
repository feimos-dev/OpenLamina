#pragma once
// A swapchain filled from memory (an RGBA8 Image), on the current session's graphics API: what an overlay set with
// SetOverlayRaw shows, or anything else OpenComposite draws itself.

#include "../OpenOVR/Misc/Image.h"

#include <openxr/openxr.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// One graphics API's part of a CpuSwapchain (CpuSwapchain_<API>.cpp)
class CpuSwapchainApi {
public:
	virtual ~CpuSwapchainApi() = default;
	// The formats it can fill from RGBA8, best first, each with whether its texels are BGRA (the bytes are swapped)
	virtual std::vector<std::pair<int64_t, bool>> Formats() const = 0;
	// Takes the images of a new swapchain of width x height (xrEnumerateSwapchainImages): false if it can't use them
	virtual bool TakeImages(XrSwapchain chain, uint32_t width, uint32_t height, int64_t format) = 0;
	// Writes rgba (RGBA8 or BGRA8 as Formats said, rows top to bottom) into image index, which is acquired and waited
	// for: false if it couldn't
	virtual bool Write(uint32_t index, const std::vector<uint8_t>& texels) = 0;
	// Its images (the swapchain is going)
	virtual void DropImages() = 0;
};

class CpuSwapchain {
public:
	// On the current session's graphics API; nullptr, with a soft failure, without a session that has one
	static std::unique_ptr<CpuSwapchain> Create(uint32_t width, uint32_t height);
	bool Upload(const Image& image); // acquire, wait, copy, release; resizes when the image's size changed
	XrSwapchainSubImage SubImage() const;
	~CpuSwapchain();

	// Whether it has a swapchain on the current session (none after ReleaseAll, until the next Upload)
	bool Live() const { return chain != XR_NULL_HANDLE; }

	// The session is ending: every CpuSwapchain lets go of its swapchain and graphics objects now, while the session
	// and its device are still there. The next Upload makes them again, on the next session.
	static void ReleaseAll();

private:
	CpuSwapchain() = default;
	// A swapchain of width x height on the current session: false, logged once, if there's none to be had
	bool Make(uint32_t width, uint32_t height);
	void Release();

	uint32_t width = 0, height = 0;
	int64_t format = 0;
	bool bgra = false;
	XrSwapchain chain = XR_NULL_HANDLE;
	uint32_t imageCount = 0;
	std::unique_ptr<CpuSwapchainApi> api;
	// An image acquired whose wait timed out: waited on again at the next Upload, not acquired anew
	bool waitPending = false;
	uint32_t pendingIndex = 0;
};

// The part for the session's graphics binding (an XrGraphicsBinding*KHR), or nullptr for one it hasn't
std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainApi(const XrBaseInStructure* binding);
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainD3D11(const XrBaseInStructure* binding);
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainD3D12(const XrBaseInStructure* binding);
#endif
#ifdef SUPPORT_VK
std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainVk(const XrBaseInStructure* binding);
#endif
#ifdef SUPPORT_GL
std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainGL(const XrBaseInStructure* binding);
#endif
