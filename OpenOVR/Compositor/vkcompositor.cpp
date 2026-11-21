
#if defined(SUPPORT_VK)

#include "vkcompositor.h"
#include "generated/interfaces/vrtypes.h"
#include "stdafx.h"

#include <algorithm>

#include <vulkan/vulkan.h>

#include "../DrvOpenXR/XrBackend.h"
#include "../Misc/Config.h"
#include "../Misc/SoftFail.h"
#include "FormatTable.h"
// Required for the close(2) call for the texture shared memory on Linux
#ifndef _WIN32
#include <unistd.h>
#endif

VkCompositor::VkCompositor(const vr::Texture_t* initialTexture)
{
	auto* tex = (vr::VRVulkanTextureData_t*)initialTexture->handle;
	// Without a device nothing can be made: CopyToSwapchain answers InvalidTexture
	if (!tex || !tex->m_pDevice)
		return;

	appDevice = tex->m_pDevice;
	appQueue = tex->m_pQueue;
	physicalDevice = tex->m_pPhysicalDevice;

	VkCommandPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	// Its command buffers go to the app's queue: use the family the backend found that queue in, else the app's word
	poolInfo.queueFamilyIndex = tex->m_nQueueFamilyIndex;
	auto* backend = dynamic_cast<XrBackend*>(BackendManager::Instance().GetBackendInstance());
	auto* binding = backend ? (const XrGraphicsBindingVulkanKHR*)backend->GetCurrentGraphicsBinding() : nullptr;
	if (binding && binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR && binding->device == tex->m_pDevice) {
		VkQueue queue = VK_NULL_HANDLE;
		vkGetDeviceQueue(binding->device, binding->queueFamilyIndex, binding->queueIndex, &queue);
		if (queue == tex->m_pQueue)
			poolInfo.queueFamilyIndex = binding->queueFamilyIndex;
		// The runtime's handle for the GPU (the game's may be of another of its instances)
		physicalDevice = binding->physicalDevice;
	}
	// Without one nothing can be copied: every Submit then answers RequestFailed
	if (vkCreateCommandPool(tex->m_pDevice, &poolInfo, nullptr, &appCommandPool) != VK_SUCCESS) {
		OOVR_LOG("vkCreateCommandPool failed: Vulkan textures can't be copied");
		appCommandPool = VK_NULL_HANDLE;
	}
	poolFamily = poolInfo.queueFamilyIndex;
}

bool VkCompositor::MakeTransferImage(VkFormat format, uint32_t width, uint32_t height, VkImage& image, VkDeviceMemory& memory)
{
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent = { width, height, 1 };
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(appDevice, &info, nullptr, &image) != VK_SUCCESS) {
		image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(appDevice, image, &requirements);
	VkMemoryAllocateInfo allocate{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = VkFindMemoryType(physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (allocate.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(appDevice, &allocate, nullptr, &memory) != VK_SUCCESS)
		memory = VK_NULL_HANDLE;
	if (!memory || vkBindImageMemory(appDevice, image, memory, 0) != VK_SUCCESS) {
		vkDestroyImage(appDevice, image, nullptr);
		if (memory)
			vkFreeMemory(appDevice, memory, nullptr);
		image = VK_NULL_HANDLE;
		memory = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

void VkCompositor::DestroyTransferImage(VkImage& image, VkDeviceMemory& memory)
{
	if (!image && !memory)
		return;
	// Frames copied through it may still be on the queue
	vkQueueWaitIdle(appQueue);
	if (image)
		vkDestroyImage(appDevice, image, nullptr);
	if (memory)
		vkFreeMemory(appDevice, memory, nullptr);
	image = VK_NULL_HANDLE;
	memory = VK_NULL_HANDLE;
}

bool VkCompositor::CreateResolveImage(VkFormat format, uint32_t width, uint32_t height)
{
	// A resolve writes it and a blit reads it
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
	const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if ((properties.optimalTilingFeatures & needed) != needed)
		return false;
	return MakeTransferImage(format, width, height, resolveImage, resolveMemory);
}

bool VkCompositor::CreateStage(VkFormat format, uint32_t width, uint32_t height)
{
	VkFormatProperties properties{};
	if (format != VK_FORMAT_UNDEFINED)
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
	if (FormatIsSrgb(GfxApiKind::Vulkan, (int64_t)format) || !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
		return false;
	return MakeTransferImage(format, width, height, stageImage, stageMemory);
}

void VkCompositor::DestroyStage()
{
	DestroyTransferImage(stageImage, stageMemory);
	DestroyTransferImage(resolveImage, resolveMemory);
	resolveFailed = false;
}

VkCompositor::~VkCompositor()
{
	FreeImageBuffers();
	DestroyStage();
	// destroying command pool also frees command buffers
	if (appCommandPool && !buffersLeft)
		vkDestroyCommandPool(appDevice, appCommandPool, nullptr);
}

void VkCompositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye>, vr::EVRSubmitFlags submitFlags)
{
	const vr::VRVulkanTextureData_t* tex = (vr::VRVulkanTextureData_t*)texture->handle;

	// XR_ERROR_VALIDATION_FAILURE: Submit answers InvalidTexture
	if (!tex || !tex->m_pDevice || !tex->m_pQueue || !tex->m_nImage) {
		OOVR_LOG_ONCE("Cannot use NULL Vulkan image data (VRVulkanTextureData_t)");
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		return;
	}
	// A sample count left 0 is a single-sampled image
	const uint32_t sampleCount = std::max(1u, tex->m_nSampleCount);

	// The command buffers are of the first texture's device and queue family
	if (tex->m_pDevice != appDevice) {
		OOVR_LOG_ONCE("A Vulkan texture of another device than the first one: not used");
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		return;
	}
	// A texture on another queue of the same family is copied on the session's queue, which the runtime syncs with,
	// after the game's work on its own queue. A queue of another family would need the images handed across families:
	// not used.
	const bool otherQueue = tex->m_pQueue != appQueue;
	if (otherQueue && tex->m_nQueueFamilyIndex != poolFamily) {
		OOVR_LOG_ONCEF("A Vulkan texture on a queue of family %u, not the first texture's %u: not used", tex->m_nQueueFamilyIndex, poolFamily);
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		return;
	}
	if (otherQueue)
		OOVR_LOG_ONCE("A Vulkan texture on another queue than the first one: copied after the game's work on it");
	if (!appCommandPool) {
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}

	// The bounds' area, copied to the origin of an image of the texture's size
	TextureRegion area;
	if (!RegionFromBounds(bounds, tex->m_nWidth, tex->m_nHeight, area)) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A Vulkan texture of no size (%u x %u)", tex->m_nWidth, tex->m_nHeight);
	}

	// A texture that fits in the swapchain is copied to its origin, and the area shown: a smaller one (dynamic
	// resolution) keeps the swapchain, and a bigger one makes one that holds both sizes, so a size that comes and goes
	// doesn't make a new swapchain each time
	const bool sameKind = chain != XR_NULL_HANDLE && sampleCount == createSourceSamples && (int64_t)tex->m_nFormat == createInfoFormat
	    && texture->eColorSpace == createColourSpace;
	bool usable = sameKind && tex->m_nWidth <= createInfo.width && tex->m_nHeight <= createInfo.height;

	if (!usable) {
		OOVR_LOG("Generating new swap chain");
		const uint32_t width = sameKind ? std::max(tex->m_nWidth, createInfo.width) : tex->m_nWidth;
		const uint32_t height = sameKind ? std::max(tex->m_nHeight, createInfo.height) : tex->m_nHeight;

		// The copies into the old chain's images finish first (FreeImageBuffers waits for them), then it goes
		FreeImageBuffers();
		DestroyStage();
		if (chain)
			xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;

		// Make eye render buffer
		createInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		createInfo.createFlags = 0;
		createInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		createInfo.faceCount = 1;
		createInfo.width = width;
		createInfo.height = height;
		createInfo.mipCount = 1;
		// A runtime that has no multisampled swapchains of that count gets a single-sampled one, the image resolved into it
		createSourceSamples = sampleCount;
		createInfo.sampleCount = xr_main_view(XruEyeLeft).maxSwapchainSampleCount < sampleCount ? 1 : sampleCount;
		createInfo.arraySize = 1;

		// The runtime's format for it: its own, or one it's blitted into (vkCmdBlitImage converts)
		std::optional<FormatChoice> choice = ChooseSwapchainFormat(GfxApiKind::Vulkan, (int64_t)tex->m_nFormat, texture->eColorSpace, RuntimeFormats());
		if (!choice) {
			// Submit answers TextureUsesUnsupportedFormat; the eye is left out
			chain = XR_NULL_HANDLE;
			createInfo = {};
			lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
			OC_UNSUPPORTED_VOID("A Vulkan image of format %d: no swapchain format can show it", (int)tex->m_nFormat);
		}
		createInfo.format = choice->swapchainFormat;
		convertFormat = choice->convert;
		createInfoFormat = (int64_t)tex->m_nFormat;
		createColourSpace = texture->eColorSpace;
		if (convertFormat) {
			// A blit converts between formats that have it; a multisampled image is resolved into an image of its format
			// for it first
			VkFormatProperties from{}, to{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, (VkFormat)tex->m_nFormat, &from);
			vkGetPhysicalDeviceFormatProperties(physicalDevice, (VkFormat)createInfo.format, &to);
			if ((tex->m_nSampleCount > 1 && !CreateResolveImage((VkFormat)tex->m_nFormat, width, height))
			    || !(from.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)
			    || !(to.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
				DestroyStage();
				// Submit answers InvalidTexture; the eye is left out
				chain = XR_NULL_HANDLE;
				createInfo = {};
				lastResult = XR_ERROR_VALIDATION_FAILURE;
				OC_UNSUPPORTED_VOID("A Vulkan image of format %d can't be blitted into swapchain format %d", (int)tex->m_nFormat, (int)choice->swapchainFormat);
			}
			OOVR_LOGF("Vulkan format %d goes into swapchain format %d through a blit", (int)tex->m_nFormat, (int)createInfo.format);
			createInfo.sampleCount = 1;
			// sRGB values in a format that isn't an sRGB one, into an sRGB swapchain: through the stage image, so the
			// blit doesn't encode them again
			if (FormatIsSrgb(GfxApiKind::Vulkan, createInfo.format) && !FormatIsSrgb(GfxApiKind::Vulkan, (int64_t)tex->m_nFormat)
			    && WantsSrgb(GfxApiKind::Vulkan, (int64_t)tex->m_nFormat, texture->eColorSpace)
			    && !CreateStage((VkFormat)FormatWithSrgb(GfxApiKind::Vulkan, createInfo.format, false), width, height))
				OOVR_LOGF("No image of swapchain format %d's linear sibling to blit into: the blit encodes the values again, and they show too bright", (int)createInfo.format);
		}

		ForgetAcquiredImage(); // a new swapchain: no image of the old one is pending
		if (SwapchainCallFailed(xrCreateSwapchain(xr_session.get(), &createInfo, &chain), "xrCreateSwapchain")) {
			chain = XR_NULL_HANDLE; // tried again at the next Submit
			return;
		}

		uint32_t chainLength = 0;
		bool listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &chainLength, nullptr), "xrEnumerateSwapchainImages");
		if (listed) {
			swapchainImages.resize(chainLength);
			for (XrSwapchainImageVulkanKHR& swapchainImage : swapchainImages)
				swapchainImage.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
			listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, swapchainImages.size(), &chainLength, (XrSwapchainImageBaseHeader*)swapchainImages.data()), "xrEnumerateSwapchainImages");
		}
		if (!listed) {
			swapchainImages.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			return;
		}

		if (!AllocateImageBuffers(chainLength)) {
			OOVR_LOG("Vulkan command buffers for the swapchain couldn't be made: this eye is left out");
			swapchainImages.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			lastResult = XR_ERROR_RUNTIME_FAILURE;
			return;
		}
	}

	copiedArea = { (int32_t)area.Width(), (int32_t)area.Height() };

	// Reserve an image from the swapchain and wait until the compositor is done with it (at most 500 ms)
	uint32_t currentIndex = 0;
	if (!AcquireAndWait((uint32_t)std::min(appCommandBuffers.size(), swapchainImages.size()), currentIndex))
		return;

	const VkCommandBuffer currentCommandBuffer = appCommandBuffers.at(currentIndex);
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	// The buffer's last submission has to be over before it's recorded again
	if (!WaitForImageBuffer(currentIndex)) {
		xrReleaseSwapchainImage(chain, &releaseInfo);
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	// A failed Vulkan call still gives the image back, unwritten, so the swapchain isn't stuck
	auto vkFailed = [&](VkResult res, const char* what) {
		if (res == VK_SUCCESS)
			return false;
		OOVR_LOGF("%s failed: %d (this eye is left out)", what, (int)res);
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		xrReleaseSwapchainImage(chain, &releaseInfo);
		return true;
	};

	if (vkFailed(vkBeginCommandBuffer(currentCommandBuffer, &beginInfo), "vkBeginCommandBuffer"))
		return;

	// transition swapchain image to TRANSFER_DST for copy
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

	barrier.image = swapchainImages.at(currentIndex).image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

	vkCmdPipelineBarrier(
	    currentCommandBuffer,
	    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    0,
	    0, nullptr,
	    0, nullptr,
	    1, &barrier);

	// The layer shown: SubmitWithArrayIndex's, else the one the game's array data names
	uint32_t arrayLayer = submitSlice ? *submitSlice : 0;
	if (!submitSlice && (submitFlags & vr::Submit_VulkanTextureWithArrayData)) {
		auto tex = static_cast<vr::VRVulkanTextureArrayData_t*>(texture->handle);
		arrayLayer = tex->m_unArrayIndex;
	}
	// A blit converts, and flips where the bounds run backwards (invertUsingShaders: always used). A multisampled image
	// is resolved into the resolve image first, which the blit reads; into a multisampled swapchain it can only be
	// copied, as it is.
	const bool flip = area.flipX || area.flipY;
	const bool wantBlit = convertFormat || flip || oovr_global_configuration.InvertUsingShaders();
	const bool multisampled = tex->m_nSampleCount > 1;
	if (wantBlit && multisampled && createInfo.sampleCount == 1 && !resolveImage && !resolveFailed) {
		resolveFailed = !CreateResolveImage((VkFormat)tex->m_nFormat, createInfo.width, createInfo.height);
		if (resolveFailed)
			OOVR_LOGF("No image to resolve a multisampled Vulkan image of format %d into for a blit", (int)tex->m_nFormat);
	}
	const bool resolveFirst = wantBlit && multisampled && resolveImage;
	if (flip && multisampled && !resolveFirst)
		OOVR_LOG_ONCE("A multisampled Vulkan image with flipped bounds: shown as it is, not flipped");
	if (wantBlit && (!multisampled || resolveFirst)) {
		VkImage blitSource = (VkImage)tex->m_nImage;
		uint32_t blitLayer = arrayLayer;
		if (resolveFirst) {
			// The area, resolved to the same place in the resolve image (whose contents before don't matter)
			VkImageMemoryBarrier toDst = barrier;
			toDst.image = resolveImage;
			toDst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
			toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			// After the last frame's blit from it, which may be on the queue still
			vkCmdPipelineBarrier(currentCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
			VkImageResolve resolve = {};
			resolve.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, arrayLayer, 1 };
			resolve.srcOffset = { (int32_t)area.x0, (int32_t)area.y0, 0 };
			resolve.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			resolve.dstOffset = { (int32_t)area.x0, (int32_t)area.y0, 0 };
			resolve.extent = { area.Width(), area.Height(), 1 };
			vkCmdResolveImage(currentCommandBuffer, (VkImage)tex->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resolveImage,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolve);
			VkImageMemoryBarrier toSrc = toDst;
			toSrc.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			toSrc.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			vkCmdPipelineBarrier(currentCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);
			blitSource = resolveImage;
			blitLayer = 0;
		}
		VkImageBlit region = {};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = 0;
		region.srcSubresource.baseArrayLayer = blitLayer;
		region.srcSubresource.layerCount = 1;
		// Reversed offsets flip
		region.srcOffsets[0] = { (int)(area.flipX ? area.x1 : area.x0), (int)(area.flipY ? area.y1 : area.y0), 0 };
		region.srcOffsets[1] = { (int)(area.flipX ? area.x0 : area.x1), (int)(area.flipY ? area.y0 : area.y1), 1 };
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount = 1;
		region.dstOffsets[0] = { 0, 0, 0 };
		region.dstOffsets[1] = { (int)area.Width(), (int)area.Height(), 1 };
		// Into the stage image where there is one (its contents before don't matter)
		VkImageMemoryBarrier stage = barrier;
		stage.image = stageImage;
		if (stageImage) {
			stage.srcAccessMask = 0;
			stage.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			stage.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			stage.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			vkCmdPipelineBarrier(currentCommandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &stage);
		}
		vkCmdBlitImage( //
		    currentCommandBuffer, // commandbuffer
		    blitSource, // srcImage
		    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // srcImageLayout
		    stageImage ? stageImage : swapchainImages.at(currentIndex).image, // dstImage
		    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // dstImageLayout
		    1, // regionCount
		    &region, // pRegions
		    VK_FILTER_NEAREST);
		if (stageImage) {
			// And on into the swapchain image, texel for texel: a copy between formats of one size converts nothing
			stage.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			stage.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			stage.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			stage.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			vkCmdPipelineBarrier(currentCommandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &stage);
			VkImageCopy whole = {};
			whole.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			whole.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			whole.extent = { area.Width(), area.Height(), 1 };
			vkCmdCopyImage(currentCommandBuffer, stageImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImages.at(currentIndex).image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
		}
	} else {
		VkImageCopy region = {};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.mipLevel = 0;
		region.srcSubresource.baseArrayLayer = arrayLayer;
		region.srcSubresource.layerCount = 1;
		region.srcOffset = { (int32_t)area.x0, (int32_t)area.y0, 0 };
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.mipLevel = 0;
		region.dstSubresource.baseArrayLayer = 0;
		region.dstSubresource.layerCount = 1;
		region.dstOffset = { 0, 0, 0 };
		region.extent = { area.Width(), area.Height(), 1 };

		bool image_is_multisampled = xr_main_view(XruEyeLeft).maxSwapchainSampleCount < tex->m_nSampleCount;

		if (image_is_multisampled) {
			// The swapchain has one sample (the runtime has no multisampling of the game's count): resolve into it.
			// (This doesn't support depth textures.)

			vkCmdResolveImage( //
			    currentCommandBuffer, // commandbuffer
			    (VkImage)tex->m_nImage, // srcImage
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // srcImageLayout
			    swapchainImages.at(currentIndex).image, // dstImage
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // dstImageLayout
			    1, // regionCount
			    (VkImageResolve*)&region // pRegions
			);
		} else {
			vkCmdCopyImage( //
			    currentCommandBuffer, // commandbuffer
			    (VkImage)tex->m_nImage, // srcImage
			    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, // srcImageLayout
			    swapchainImages.at(currentIndex).image, // dstImage
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, // dstImageLayout
			    1, // regionCount
			    &region // pRegions
			);
		}
	}

	// transition swapchain image back to COLOR_ATTACHMENT_OPTIMAL for runtime
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(
	    currentCommandBuffer, //
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    0,
	    0, nullptr,
	    0, nullptr,
	    1, &barrier);

	if (vkFailed(vkEndCommandBuffer(currentCommandBuffer), "vkEndCommandBuffer"))
		return;

	VkSubmitInfo submitInfo = {};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &currentCommandBuffer;

	// A texture on another queue: the copy waits for a semaphore that queue signals after the game's work
	const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	if (otherQueue) {
		VkSubmitInfo signal = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		signal.signalSemaphoreCount = 1;
		signal.pSignalSemaphores = &imageSemaphores.at(currentIndex);
		if (vkFailed(vkQueueSubmit(tex->m_pQueue, 1, &signal, VK_NULL_HANDLE), "vkQueueSubmit (the texture's queue)"))
			return;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &imageSemaphores.at(currentIndex);
		submitInfo.pWaitDstStageMask = &waitStage;
	}

	if (vkFailed(SubmitWithImageFence(appQueue, submitInfo, currentIndex) ? VK_SUCCESS : VK_ERROR_UNKNOWN, "vkQueueSubmit")) {
		if (otherQueue) {
			// The semaphore is waited for anyway, so it isn't left signalled
			VkSubmitInfo drain = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			drain.waitSemaphoreCount = 1;
			drain.pWaitSemaphores = &imageSemaphores.at(currentIndex);
			drain.pWaitDstStageMask = &waitStage;
			SubmitWithImageFence(appQueue, drain, currentIndex);
		}
		return;
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage");
}

bool VkCompositor::CopyDepth(const vr::Texture_t* depth)
{
	// The game's depth image is in TRANSFER_SRC_OPTIMAL (as its colour is); the runtime's go back in
	// DEPTH_STENCIL_ATTACHMENT_OPTIMAL
	const auto* tex = (const vr::VRVulkanTextureData_t*)depth->handle;
	if (!tex->m_nImage || tex->m_pDevice != appDevice || tex->m_pQueue != appQueue || !appCommandPool)
		return false;
	if (tex->m_nSampleCount > 1) {
		OOVR_LOG_ONCE("A multisampled Vulkan depth image: not sent to the runtime");
		return false;
	}
	if (!chain || createInfo.width != tex->m_nWidth || createInfo.height != tex->m_nHeight || createInfoFormat != (int64_t)tex->m_nFormat) {
		std::optional<int64_t> format = ChooseDepthSwapchainFormat(GfxApiKind::Vulkan, (int64_t)tex->m_nFormat, RuntimeFormats());
		if (!format) {
			OOVR_LOG_ONCEF("No depth swapchain format of the runtime's for a Vulkan depth image of format %d: depth not sent", (int)tex->m_nFormat);
			return false;
		}
		if (!appCommandBuffers.empty()) {
			vkQueueWaitIdle(appQueue);
			FreeImageBuffers();
		}
		if (!MakeDepthChain(*format, tex->m_nWidth, tex->m_nHeight))
			return false;
		createInfoFormat = (int64_t)tex->m_nFormat;
		uint32_t count = 0;
		bool listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &count, nullptr), "xrEnumerateSwapchainImages (depth)");
		if (listed) {
			swapchainImages.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
			listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)swapchainImages.data()), "xrEnumerateSwapchainImages (depth)");
		}
		if (!listed || !AllocateImageBuffers(count)) {
			swapchainImages.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			createInfo = {};
			return false;
		}
	}
	uint32_t index = 0;
	if (!AcquireAndWait((uint32_t)std::min(appCommandBuffers.size(), swapchainImages.size()), index))
		return false;
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	VkCommandBuffer cmd = appCommandBuffers[index];
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (!WaitForImageBuffer(index)) {
		xrReleaseSwapchainImage(chain, &releaseInfo);
		return false;
	}
	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		xrReleaseSwapchainImage(chain, &releaseInfo);
		return false;
	}
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = swapchainImages[index].image;
	// A barrier on an image of a depth-stencil format names both aspects
	const VkFormat depthFormat = (VkFormat)createInfo.format;
	const bool stencil = depthFormat == VK_FORMAT_D16_UNORM_S8_UINT || depthFormat == VK_FORMAT_D24_UNORM_S8_UINT || depthFormat == VK_FORMAT_D32_SFLOAT_S8_UINT;
	barrier.subresourceRange = { (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, 1, 0, 1 };
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	// A depth image is copied whole: mip 0, layer 0
	VkImageCopy region = {};
	region.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
	region.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
	region.extent = { tex->m_nWidth, tex->m_nHeight, 1 };
	vkCmdCopyImage(cmd, (VkImage)tex->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImages[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	if (vkEndCommandBuffer(cmd) != VK_SUCCESS || !SubmitWithImageFence(appQueue, submitInfo, index)) {
		xrReleaseSwapchainImage(chain, &releaseInfo);
		return false;
	}
	return !SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage (depth)");
}

bool VkCompositor::AllocateImageBuffers(uint32_t count)
{
	appCommandBuffers.resize(count);
	VkCommandBufferAllocateInfo bufInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	bufInfo.commandPool = appCommandPool;
	bufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	bufInfo.commandBufferCount = count;
	if (vkAllocateCommandBuffers(appDevice, &bufInfo, appCommandBuffers.data()) != VK_SUCCESS) {
		appCommandBuffers.clear();
		return false;
	}
	// Signalled, as if their last submission were over
	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	imageFences.assign(count, VK_NULL_HANDLE);
	for (VkFence& fence : imageFences) {
		if (vkCreateFence(appDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
			FreeImageBuffers();
			return false;
		}
	}
	VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	imageSemaphores.assign(count, VK_NULL_HANDLE);
	for (VkSemaphore& semaphore : imageSemaphores) {
		if (vkCreateSemaphore(appDevice, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
			FreeImageBuffers();
			return false;
		}
	}
	return true;
}

void VkCompositor::FreeImageBuffers()
{
	// Not freed under a submission that still runs: one that hasn't ended in a second leaves them all (leaked, logged)
	bool done = true;
	for (VkFence fence : imageFences) {
		if (fence && vkWaitForFences(appDevice, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
			done = false;
	}
	if (!done) {
		OOVR_LOG_ONCE("A Vulkan copy didn't finish in time: its command buffers and fences are left");
		buffersLeft = true;
		imageFences.clear();
		imageSemaphores.clear();
		appCommandBuffers.clear();
		return;
	}
	for (VkFence fence : imageFences) {
		if (fence)
			vkDestroyFence(appDevice, fence, nullptr);
	}
	imageFences.clear();
	for (VkSemaphore semaphore : imageSemaphores) {
		if (semaphore)
			vkDestroySemaphore(appDevice, semaphore, nullptr);
	}
	imageSemaphores.clear();
	if (!appCommandBuffers.empty() && appCommandPool)
		vkFreeCommandBuffers(appDevice, appCommandPool, (uint32_t)appCommandBuffers.size(), appCommandBuffers.data());
	appCommandBuffers.clear();
}

bool VkCompositor::WaitForImageBuffer(uint32_t index)
{
	if (index >= imageFences.size())
		return false;
	VkFence fence = imageFences[index];
	if (!fence || vkWaitForFences(appDevice, 1, &fence, VK_TRUE, 500000000ull) != VK_SUCCESS) {
		OOVR_LOG_ONCE("A Vulkan copy didn't finish within 0.5 s: frame left out");
		return false;
	}
	return true;
}

bool VkCompositor::SubmitWithImageFence(VkQueue queue, const VkSubmitInfo& submitInfo, uint32_t index)
{
	// Reset only now, so a failure on the way here leaves it signalled for the next copy with this buffer
	VkFence& fence = imageFences.at(index);
	if (vkResetFences(appDevice, 1, &fence) != VK_SUCCESS)
		return false;
	if (vkQueueSubmit(queue, 1, &submitInfo, fence) == VK_SUCCESS)
		return true;
	// Nothing will signal it now: a signalled one in its place
	vkDestroyFence(appDevice, fence, nullptr);
	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(appDevice, &fenceInfo, nullptr, &fence) != VK_SUCCESS)
		fence = VK_NULL_HANDLE;
	return false;
}

uint32_t VkFindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags properties)
{
	VkPhysicalDeviceMemoryProperties mem{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mem);
	for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
		if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & properties) == properties)
			return i;
	return UINT32_MAX;
}

TexelLayout TexelLayoutOfVk(int64_t format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		return TexelLayout::RGBA8;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		return TexelLayout::BGRA8;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return TexelLayout::RGBA16F;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return TexelLayout::RGB10A2;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		return TexelLayout::RG11B10F;
	case VK_FORMAT_D32_SFLOAT:
		return TexelLayout::R32F;
	default:
		return TexelLayout::Unknown;
	}
}

bool VkCompositor::RunOnce(const std::function<void(VkCommandBuffer)>& record)
{
	if (!appCommandPool)
		return false;
	VkCommandBufferAllocateInfo bufInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	bufInfo.commandPool = appCommandPool;
	bufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	bufInfo.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(appDevice, &bufInfo, &cmd) != VK_SUCCESS)
		return false;
	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	bool ok = vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
	if (ok) {
		record(cmd);
		ok = vkEndCommandBuffer(cmd) == VK_SUCCESS;
	}
	VkFence fence = VK_NULL_HANDLE;
	VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	ok = ok && vkCreateFence(appDevice, &fenceInfo, nullptr, &fence) == VK_SUCCESS;
	if (ok) {
		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		ok = vkQueueSubmit(appQueue, 1, &submitInfo, fence) == VK_SUCCESS;
		if (ok && vkWaitForFences(appDevice, 1, &fence, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
			// Still running: its command buffer and fence are left to the pool's end, not freed under it
			OOVR_LOG_ONCE("A one-time Vulkan command buffer didn't finish within 5 s");
			return false;
		}
	}
	if (fence)
		vkDestroyFence(appDevice, fence, nullptr);
	vkFreeCommandBuffers(appDevice, appCommandPool, 1, &cmd);
	return ok;
}

bool VkCompositor::ReadbackToCpu(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, Image& out)
{
	// The game's image is in TRANSFER_SRC_OPTIMAL, as OpenVR's Vulkan textures are given, and stays in it
	const auto* tex = texture ? (const vr::VRVulkanTextureData_t*)texture->handle : nullptr;
	if (!tex || !tex->m_nImage || tex->m_pDevice != appDevice || !tex->m_pPhysicalDevice)
		return false;
	const TexelLayout layout = TexelLayoutOfVk(tex->m_nFormat);
	const uint32_t bytes = BytesOfTexelLayout(layout);
	if (!bytes) {
		OOVR_LOG_ONCEF("A Vulkan image of format %d can't be read back into memory", (int)tex->m_nFormat);
		return false;
	}
	TextureRegion region;
	if (!RegionFromBounds(bounds, tex->m_nWidth, tex->m_nHeight, region))
		return false;

	struct Owned {
		VkDevice dev;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
		VkImage resolved = VK_NULL_HANDLE;
		VkDeviceMemory resolvedMemory = VK_NULL_HANDLE;
		bool keep = false; // a timed-out copy may still use them
		~Owned()
		{
			if (keep)
				return;
			if (buffer)
				vkDestroyBuffer(dev, buffer, nullptr);
			if (bufferMemory)
				vkFreeMemory(dev, bufferMemory, nullptr);
			if (resolved)
				vkDestroyImage(dev, resolved, nullptr);
			if (resolvedMemory)
				vkFreeMemory(dev, resolvedMemory, nullptr);
		}
	} owned{ appDevice };

	auto allocate = [&](VkMemoryRequirements req, VkMemoryPropertyFlags props, VkDeviceMemory& memory) {
		VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.allocationSize = req.size;
		ai.memoryTypeIndex = VkFindMemoryType(tex->m_pPhysicalDevice, req.memoryTypeBits, props);
		return ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(appDevice, &ai, nullptr, &memory) == VK_SUCCESS;
	};

	const VkDeviceSize size = (VkDeviceSize)region.Width() * region.Height() * bytes;
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(appDevice, &bi, nullptr, &owned.buffer) != VK_SUCCESS)
		return false;
	VkMemoryRequirements req{};
	vkGetBufferMemoryRequirements(appDevice, owned.buffer, &req);
	if (!allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, owned.bufferMemory)
	    || vkBindBufferMemory(appDevice, owned.buffer, owned.bufferMemory, 0) != VK_SUCCESS)
		return false;

	const bool depth = layout == TexelLayout::R32F;
	const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	VkImage from = (VkImage)tex->m_nImage;
	// A multisampled image is resolved first, into an image of its own
	if (tex->m_nSampleCount > 1) {
		VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = (VkFormat)tex->m_nFormat;
		ici.extent = { tex->m_nWidth, tex->m_nHeight, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (depth || vkCreateImage(appDevice, &ici, nullptr, &owned.resolved) != VK_SUCCESS)
			return false;
		vkGetImageMemoryRequirements(appDevice, owned.resolved, &req);
		if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, owned.resolvedMemory)
		    || vkBindImageMemory(appDevice, owned.resolved, owned.resolvedMemory, 0) != VK_SUCCESS)
			return false;
	}

	bool ran = RunOnce([&](VkCommandBuffer cmd) {
		VkImage src = from;
		if (owned.resolved) {
			VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
			b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.image = owned.resolved;
			b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
			b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			VkImageResolve r{};
			r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			r.extent = { tex->m_nWidth, tex->m_nHeight, 1 };
			vkCmdResolveImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, owned.resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
			b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
			src = owned.resolved;
		}
		VkBufferImageCopy copy{};
		copy.imageSubresource = { aspect, 0, 0, 1 };
		copy.imageOffset = { (int32_t)region.x0, (int32_t)region.y0, 0 };
		copy.imageExtent = { region.Width(), region.Height(), 1 };
		vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, owned.buffer, 1, &copy);
		VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
		hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		hb.buffer = owned.buffer;
		hb.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hb, 0, nullptr);
	});
	if (!ran) {
		owned.keep = true;
		return false;
	}
	void* mapped = nullptr;
	if (vkMapMemory(appDevice, owned.bufferMemory, 0, size, 0, &mapped) != VK_SUCCESS)
		return false;
	out = DecodeTexels(layout, mapped, region.Width(), region.Height(), (size_t)region.Width() * bytes, false,
	    TexelsAreLinear(layout, texture->eColorSpace == vr::ColorSpace_Gamma));
	vkUnmapMemory(appDevice, owned.bufferMemory);
	FlipImage(out, region.flipX, region.flipY);
	return !out.Empty();
}

#endif
