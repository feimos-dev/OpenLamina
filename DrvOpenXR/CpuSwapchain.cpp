#include "CpuSwapchain.h"

#include "XrBackend.h"

#include "../OpenOVR/Drivers/Backend.h"
#include "../OpenOVR/Misc/SoftFail.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace {

// Every CpuSwapchain alive, for ReleaseAll
std::mutex& LiveLock()
{
	static std::mutex lock;
	return lock;
}
std::set<CpuSwapchain*>& LiveSet()
{
	static std::set<CpuSwapchain*> live;
	return live;
}

const XrBaseInStructure* CurrentBinding()
{
	auto* backend = (XrBackend*)BackendManager::Instance().GetBackendInstance();
	if (!backend || !xr_session.get())
		return nullptr;
	return (const XrBaseInStructure*)backend->GetCurrentGraphicsBinding();
}

} // namespace

std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainApi(const XrBaseInStructure* binding)
{
	if (!binding)
		return nullptr;
	switch (binding->type) {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	case XR_TYPE_GRAPHICS_BINDING_D3D11_KHR:
		return MakeCpuSwapchainD3D11(binding);
#endif
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
	case XR_TYPE_GRAPHICS_BINDING_D3D12_KHR:
		return MakeCpuSwapchainD3D12(binding);
#endif
#ifdef SUPPORT_VK
	case XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR:
		return MakeCpuSwapchainVk(binding);
#endif
#ifdef SUPPORT_GL
#ifdef _WIN32
	case XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR:
#else
	case XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR:
#endif
		return MakeCpuSwapchainGL(binding);
#endif
	default:
		return nullptr;
	}
}

std::unique_ptr<CpuSwapchain> CpuSwapchain::Create(uint32_t width, uint32_t height)
{
	auto lock = xr_session.lock_shared();
	if (!CurrentBinding())
		OC_UNSUPPORTED(nullptr, "Images from memory need a session with a graphics API: none here");
	std::unique_ptr<CpuSwapchain> made(new CpuSwapchain());
	if (!made->Make(width, height))
		return nullptr;
	std::lock_guard<std::mutex> live(LiveLock());
	LiveSet().insert(made.get());
	return made;
}

CpuSwapchain::~CpuSwapchain()
{
	{
		std::lock_guard<std::mutex> live(LiveLock());
		LiveSet().erase(this);
	}
	auto lock = xr_session.lock_shared();
	Release();
}

void CpuSwapchain::ReleaseAll()
{
	std::lock_guard<std::mutex> live(LiveLock());
	for (CpuSwapchain* c : LiveSet())
		c->Release();
}

void CpuSwapchain::Release()
{
	if (api)
		api->DropImages();
	api.reset();
	if (chain)
		xrDestroySwapchain(chain);
	chain = XR_NULL_HANDLE;
	imageCount = 0;
	waitPending = false;
	filled = false;
}

bool CpuSwapchain::Make(uint32_t w, uint32_t h)
{
	Release();
	width = w;
	height = h;
	if (!w || !h)
		return false;
	api = MakeCpuSwapchainApi(CurrentBinding());
	if (!api) {
		OOVR_LOG_ONCE("Images from memory: no swapchain of this session's graphics API can be filled from memory");
		return false;
	}
	// The API's best format the runtime offers
	static const std::vector<int64_t> none;
	const std::vector<int64_t>& offered = xr_gbl ? xr_gbl->SwapchainFormats() : none;
	bool found = false;
	for (const auto& [f, swapped] : api->Formats()) {
		if (std::find(offered.begin(), offered.end(), f) != offered.end()) {
			format = f;
			bgra = swapped;
			found = true;
			break;
		}
	}
	if (!found) {
		OOVR_LOG_ONCE("Images from memory: the runtime offers no 8-bit RGBA or BGRA swapchain format");
		api.reset();
		return false;
	}

	XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	info.format = format;
	info.sampleCount = 1;
	info.width = w;
	info.height = h;
	info.faceCount = 1;
	info.arraySize = 1;
	info.mipCount = 1;
	if (XrResult res = xrCreateSwapchain(xr_session.get(), &info, &chain); XR_FAILED(res)) {
		OOVR_LOG_ONCEF("Images from memory: xrCreateSwapchain failed: %d", (int)res);
		chain = XR_NULL_HANDLE;
		api.reset();
		return false;
	}
	if (XR_FAILED(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr)) || !imageCount || !api->TakeImages(chain, w, h, format)) {
		OOVR_LOG_ONCE("Images from memory: the swapchain's images can't be used");
		Release();
		return false;
	}
	return true;
}

bool CpuSwapchain::Upload(const Image& image)
{
	if (image.Empty() || image.rgba.size() < (size_t)image.width * image.height * 4)
		return false;
	auto lock = xr_session.lock_shared();
	if (!chain || image.width != width || image.height != height) {
		if (!CurrentBinding() || !Make(image.width, image.height))
			return false;
	}

	if (!waitPending) {
		XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if (XrResult res = xrAcquireSwapchainImage(chain, &acquireInfo, &pendingIndex); XR_FAILED(res)) {
			OOVR_LOG_ONCEF("Images from memory: xrAcquireSwapchainImage failed: %d", (int)res);
			return false;
		}
		waitPending = true;
	}
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // 500 ms
	XrResult waited = xrWaitSwapchainImage(chain, &waitInfo);
	if (waited == XR_TIMEOUT_EXPIRED) {
		OOVR_LOG_ONCE("Images from memory: xrWaitSwapchainImage timed out (tried again at the next upload)");
		return false;
	}
	if (XR_FAILED(waited)) {
		// The image can't be released unwaited: this swapchain goes, and the next upload makes another
		OOVR_LOG_ONCEF("Images from memory: xrWaitSwapchainImage failed: %d", (int)waited);
		Release();
		return false;
	}
	waitPending = false;

	bool written = false;
	if (pendingIndex < imageCount) {
		if (bgra) {
			std::vector<uint8_t> swapped(image.rgba);
			for (size_t i = 0; i + 3 < swapped.size(); i += 4)
				std::swap(swapped[i], swapped[i + 2]);
			written = api->Write(pendingIndex, swapped);
		} else {
			written = api->Write(pendingIndex, image.rgba);
		}
	}
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if (XR_FAILED(xrReleaseSwapchainImage(chain, &releaseInfo)))
		written = false;
	if (!written)
		OOVR_LOG_ONCE("Images from memory: an image couldn't be written into its swapchain");
	filled = filled || written;
	return written;
}

XrSwapchainSubImage CpuSwapchain::SubImage() const
{
	XrSwapchainSubImage sub{};
	sub.swapchain = chain;
	sub.imageRect.offset = { 0, 0 };
	sub.imageRect.extent = { (int32_t)width, (int32_t)height };
	sub.imageArrayIndex = 0;
	return sub;
}
