#pragma once

#include "compositor.h"

#include "../Misc/Image.h"

#include <functional>

// The first memory type of the physical device among typeBits with the properties, or UINT32_MAX
uint32_t VkFindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties);
// How a VkFormat's texels lie in memory, for reading them back
TexelLayout TexelLayoutOfVk(int64_t format);

class VkCompositor : public Compositor {
public:
	VkCompositor(const vr::Texture_t* initialTexture);

	~VkCompositor() override;

	void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	void InvokeCubemap(const vr::Texture_t* textures) override;

	bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out) override;

protected:
	bool CopyDepth(const vr::Texture_t* depth) override;

private:
	// Records into a one-time command buffer of the app's pool, runs it on the app's queue and waits (at most 5 s)
	bool RunOnce(const std::function<void(VkCommandBuffer)>& record);
	// The swapchain's format isn't the image's family: it's blitted in, which converts
	bool convertFormat = false;
	// The colour space the swapchain was made for
	vr::EColorSpace createColourSpace = vr::ColorSpace_Auto;
	// An image whose values are sRGB already, in a format that isn't an sRGB one (most games' 8-bit textures), going into
	// an sRGB swapchain of another family: a blit straight into the swapchain would encode the values once more, and
	// show them too bright. The blit fills this image of the swapchain's linear sibling instead, where the values stay
	// as they are, and that is copied on unchanged. Null when no such step is needed (or it couldn't be made).
	VkImage stageImage = VK_NULL_HANDLE;
	VkDeviceMemory stageMemory = VK_NULL_HANDLE;
	bool CreateStage(VkFormat format, uint32_t width, uint32_t height);
	void DestroyStage();
	// The GPU, as the runtime knows it
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	// These resources live in the runtime's VkDevice
	std::vector<XrSwapchainImageVulkanKHR> swapchainImages;

	// These resources live in the app's VkDevice
	VkDevice appDevice = VK_NULL_HANDLE;
	VkQueue appQueue = VK_NULL_HANDLE;
	VkCommandPool appCommandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> appCommandBuffers{};
};
