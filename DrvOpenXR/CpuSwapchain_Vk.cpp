// A CpuSwapchain's Vulkan part: the image's texels go into a host-visible buffer, and from there into the swapchain
// image with vkCmdCopyBufferToImage on the session's queue (waited for, at most 5 s). The image is handed back in
// COLOR_ATTACHMENT_OPTIMAL, as the runtime expects a colour swapchain image.

#ifdef SUPPORT_VK

#include "CpuSwapchain.h"

#include "XrDriverPrivate.h"

#include <vulkan/vulkan.h>

#include <cstring>

namespace {

uint32_t MemoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(physical, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props)
			return i;
	return UINT32_MAX;
}

class CpuSwapchainVk : public CpuSwapchainApi {
public:
	CpuSwapchainVk(VkPhysicalDevice physical, VkDevice dev, uint32_t family, uint32_t queueIndex)
	    : physicalDevice(physical), device(dev), queueFamily(family)
	{
		vkGetDeviceQueue(device, family, queueIndex, &queue);
	}
	~CpuSwapchainVk() override { DropImages(); }

	std::vector<std::pair<int64_t, bool>> Formats() const override
	{
		return { { VK_FORMAT_R8G8B8A8_SRGB, false }, { VK_FORMAT_B8G8R8A8_SRGB, true }, { VK_FORMAT_R8G8B8A8_UNORM, false },
			{ VK_FORMAT_B8G8R8A8_UNORM, true } };
	}

	bool TakeImages(XrSwapchain chain, uint32_t w, uint32_t h, int64_t) override
	{
		uint32_t count = 0;
		if (!queue || XR_FAILED(xrEnumerateSwapchainImages(chain, 0, &count, nullptr)))
			return false;
		std::vector<XrSwapchainImageVulkanKHR> list(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
		if (XR_FAILED(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)list.data())) || list.empty())
			return false;
		for (const XrSwapchainImageVulkanKHR& i : list)
			images.push_back(i.image);
		width = w;
		height = h;

		VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pi.queueFamilyIndex = queueFamily;
		if (vkCreateCommandPool(device, &pi, nullptr, &pool) != VK_SUCCESS)
			return false;
		VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS)
			return false;
		VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		if (vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS)
			return false;

		VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
		bi.size = (VkDeviceSize)w * h * 4;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(device, &bi, nullptr, &buffer) != VK_SUCCESS)
			return false;
		VkMemoryRequirements req{};
		vkGetBufferMemoryRequirements(device, buffer, &req);
		VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mi.allocationSize = req.size;
		mi.memoryTypeIndex = MemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (mi.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &mi, nullptr, &memory) != VK_SUCCESS)
			return false;
		return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
	}

	bool Write(uint32_t index, const std::vector<uint8_t>& texels) override
	{
		if (index >= images.size() || !cmd || vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS)
			return false;
		void* mapped = nullptr;
		if (vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
			return false;
		std::memcpy(mapped, texels.data(), (size_t)width * height * 4);
		vkUnmapMemory(device, memory);

		VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS || vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS)
			return false;
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = images[index];
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		VkBufferImageCopy copy{};
		copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.imageExtent = { width, height, 1 };
		vkCmdCopyBufferToImage(cmd, buffer, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		b.dstAccessMask = 0;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS || vkResetFences(device, 1, &fence) != VK_SUCCESS)
			return false;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS)
			return false;
		return vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
	}

	void DropImages() override
	{
		// Nothing of ours may still be running on them (at most 5 s; after that the objects are left, not freed under
		// the GPU)
		if (fence && vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
			images.clear();
			return;
		}
		images.clear();
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (pool)
			vkDestroyCommandPool(device, pool, nullptr);
		if (buffer)
			vkDestroyBuffer(device, buffer, nullptr);
		if (memory)
			vkFreeMemory(device, memory, nullptr);
		fence = VK_NULL_HANDLE;
		pool = VK_NULL_HANDLE;
		cmd = VK_NULL_HANDLE;
		buffer = VK_NULL_HANDLE;
		memory = VK_NULL_HANDLE;
	}

private:
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	uint32_t queueFamily;
	VkQueue queue = VK_NULL_HANDLE;
	std::vector<VkImage> images; // the runtime's
	uint32_t width = 0, height = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
};

} // namespace

std::unique_ptr<CpuSwapchainApi> MakeCpuSwapchainVk(const XrBaseInStructure* binding)
{
	auto* b = (const XrGraphicsBindingVulkanKHR*)binding;
	if (!b->device || !b->physicalDevice)
		return nullptr;
	return std::make_unique<CpuSwapchainVk>(b->physicalDevice, b->device, b->queueFamilyIndex, b->queueIndex);
}

#endif
