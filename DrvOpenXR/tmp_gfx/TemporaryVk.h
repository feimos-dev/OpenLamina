//
// Created by ZNix on 8/02/2021.
//

#pragma once

#include "TemporaryGraphics.h"

#include "../XrDriverPrivate.h"

// The first queue family that can draw (and so also copy), or -1
inline int PickGraphicsQueueFamily(const VkQueueFamilyProperties* families, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (families[i].queueCount > 0 && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			return (int)i;
	}
	return -1;
}

// Whether a Vulkan loader of this version (VK_MAKE_VERSION) meets the OpenXR runtime's minimum API version
inline bool VulkanVersionMeetsXr(uint32_t vulkanVersion, XrVersion minimum)
{
	const uint32_t major = (uint32_t)XR_VERSION_MAJOR(minimum), minor = (uint32_t)XR_VERSION_MINOR(minimum);
	return VK_VERSION_MAJOR(vulkanVersion) > major || (VK_VERSION_MAJOR(vulkanVersion) == major && VK_VERSION_MINOR(vulkanVersion) >= minor);
}

class TemporaryVk : public TemporaryGraphics {
public:
	TemporaryVk();
	~TemporaryVk() override;

	const void* GetGraphicsBinding() const override { return &binding; }

	TemporaryVk* GetAsVk() override { return this; }

private:
	XrGraphicsBindingVulkanKHR binding = {};

public:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
};
