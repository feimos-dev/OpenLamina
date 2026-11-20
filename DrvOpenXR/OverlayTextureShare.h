#pragma once
// An overlay application's Vulkan or OpenGL texture copied, on the program's thread and on its own queue or context,
// into an image of the overlay session's Vulkan device (OpenLamina's own) whose memory both share
// (VK_KHR_external_memory_fd, GL_EXT_memory_object_fd): only on the same device and driver (by their UUIDs), where the
// program's device or context has the extension. Otherwise the texture is read back into memory (BaseOverlay).

#if defined(SUPPORT_VK) && !defined(_WIN32)

#include "generated/interfaces/vrtypes.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>

class OverlayTextureShare {
public:
	OverlayTextureShare(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, VkQueue queue);
	~OverlayTextureShare();
	OverlayTextureShare(const OverlayTextureShare&) = delete;
	OverlayTextureShare& operator=(const OverlayTextureShare&) = delete;

	// On the program's thread: tex (a Vulkan texture in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, as OpenVR asks, or an
	// OpenGL texture of the context current on this thread) copied whole into owner's image of the session's device,
	// MSAA resolved. That image comes back as a Vulkan texture of the session's device, in
	// VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL and owned by its queue's family, for the session's compositor to copy from;
	// Release hands it back once that copy is submitted. flipY: its rows run bottom-up as OpenVR's bounds see them (an
	// OpenGL texture's). nullptr, with why, if it can't be shared: the caller reads the texture back instead.
	const vr::VRVulkanTextureData_t* CopyIn(const vr::Texture_t* tex, const void* owner, bool* flipY, std::string* why);
	// On the program's thread, after the session's copy from CopyIn's image was submitted: the image is handed back to
	// the program's side once that copy has run (waited for, at most 100 ms)
	void Release(const void* owner);
	void Forget(const void* owner);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

#endif
