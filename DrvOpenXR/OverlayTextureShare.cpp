#include "stdafx.h"

#include "OverlayTextureShare.h"

#if defined(SUPPORT_VK) && !defined(_WIN32)

#include "../OpenOVR/logging.h"

#ifdef SUPPORT_GL
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#endif

#include <cstring>
#include <map>
#include <unistd.h>

namespace {

// How long a copy on either side is waited for; past it the texture is read back instead
constexpr uint64_t kWaitNs = 100'000'000;

constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

// One command buffer of a queue family of a device, and its fence: for one copy at a time, waited for
struct Commands {
	VkDevice device = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer buffer = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool busy = false; // a submission that wasn't seen to finish: its buffer isn't reused until it has

	bool Make(VkDevice d, uint32_t f)
	{
		Drop();
		device = d;
		family = f;
		VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = family;
		VkCommandBufferAllocateInfo bufferInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		bufferInfo.commandBufferCount = 1;
		bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
			pool = VK_NULL_HANDLE;
			return false;
		}
		bufferInfo.commandPool = pool;
		if (vkAllocateCommandBuffers(device, &bufferInfo, &buffer) == VK_SUCCESS && vkCreateFence(device, &fenceInfo, nullptr, &fence) == VK_SUCCESS)
			return true;
		buffer = VK_NULL_HANDLE;
		Drop();
		return false;
	}
	void Drop()
	{
		if (busy && fence && vkWaitForFences(device, 1, &fence, VK_TRUE, kWaitNs) != VK_SUCCESS) {
			// Still running: left as it is rather than freed under the GPU
			OOVR_LOG("Overlay texture sharing: a copy never finished; its command buffer isn't freed");
			pool = VK_NULL_HANDLE;
			fence = VK_NULL_HANDLE;
		}
		if (fence)
			vkDestroyFence(device, fence, nullptr);
		if (pool)
			vkDestroyCommandPool(device, pool, nullptr);
		pool = VK_NULL_HANDLE;
		buffer = VK_NULL_HANDLE;
		fence = VK_NULL_HANDLE;
		busy = false;
	}
	// Records and submits on queue, and waits for it: false if it couldn't, or didn't finish in time
	template <typename F>
	bool Run(VkQueue queue, F&& record)
	{
		if (busy) {
			if (vkWaitForFences(device, 1, &fence, VK_TRUE, kWaitNs) != VK_SUCCESS)
				return false;
			busy = false;
		}
		vkResetFences(device, 1, &fence);
		vkResetCommandBuffer(buffer, 0);
		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(buffer, &begin) != VK_SUCCESS)
			return false;
		record(buffer);
		if (vkEndCommandBuffer(buffer) != VK_SUCCESS)
			return false;
		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &buffer;
		if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS)
			return false;
		busy = true;
		if (vkWaitForFences(device, 1, &fence, VK_TRUE, kWaitNs) != VK_SUCCESS)
			return false;
		busy = false;
		return true;
	}
};

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, uint32_t fromFamily, uint32_t toFamily)
{
	VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = fromFamily;
	b.dstQueueFamilyIndex = toFamily;
	b.image = image;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	return b;
}

// Memory is shared only between the same device on the same driver: both UUIDs. False if they can't be had.
bool DeviceIds(VkPhysicalDevice physicalDevice, uint8_t (&device)[VK_UUID_SIZE], uint8_t (&driver)[VK_UUID_SIZE])
{
	VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
	vkGetPhysicalDeviceProperties2(physicalDevice, &props);
	memcpy(device, id.deviceUUID, VK_UUID_SIZE);
	memcpy(driver, id.driverUUID, VK_UUID_SIZE);
	for (uint8_t b : device)
		if (b)
			return true;
	return false;
}

#ifdef SUPPORT_GL
// The Vulkan format of an OpenGL texture's internal format, for the colour formats programs give overlays
VkFormat VkFormatOfGl(GLint internalFormat)
{
	switch (internalFormat) {
	case GL_RGBA8:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case GL_SRGB8_ALPHA8:
		return VK_FORMAT_R8G8B8A8_SRGB;
	case GL_RGB10_A2:
		return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	case GL_RGBA16F:
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	case GL_RGBA32F:
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

// GL_EXT_memory_object(_fd), and what's needed around it, from the current context's driver
struct GlFunctions {
	PFNGLGETSTRINGIPROC getStringi = nullptr;
	PFNGLGETUNSIGNEDBYTEVEXTPROC getUnsignedBytev = nullptr;
	PFNGLGETUNSIGNEDBYTEI_VEXTPROC getUnsignedBytei = nullptr;
	PFNGLCREATEMEMORYOBJECTSEXTPROC createMemoryObjects = nullptr;
	PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemoryObjects = nullptr;
	PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryObjectParameteriv = nullptr;
	PFNGLIMPORTMEMORYFDEXTPROC importMemoryFd = nullptr;
	PFNGLTEXSTORAGEMEM2DEXTPROC texStorageMem2D = nullptr;
	PFNGLCOPYIMAGESUBDATAPROC copyImageSubData = nullptr;
	PFNGLFENCESYNCPROC fenceSync = nullptr;
	PFNGLCLIENTWAITSYNCPROC clientWaitSync = nullptr;
	PFNGLDELETESYNCPROC deleteSync = nullptr;
	bool loaded = false, usable = false;

	template <typename T>
	static T Get(const char* name)
	{
		return (T)glXGetProcAddress((const GLubyte*)name);
	}
	bool Load()
	{
		if (loaded)
			return usable;
		loaded = true;
		getStringi = Get<PFNGLGETSTRINGIPROC>("glGetStringi");
		getUnsignedBytev = Get<PFNGLGETUNSIGNEDBYTEVEXTPROC>("glGetUnsignedBytevEXT");
		getUnsignedBytei = Get<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>("glGetUnsignedBytei_vEXT");
		createMemoryObjects = Get<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT");
		deleteMemoryObjects = Get<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT");
		memoryObjectParameteriv = Get<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>("glMemoryObjectParameterivEXT");
		importMemoryFd = Get<PFNGLIMPORTMEMORYFDEXTPROC>("glImportMemoryFdEXT");
		texStorageMem2D = Get<PFNGLTEXSTORAGEMEM2DEXTPROC>("glTexStorageMem2DEXT");
		copyImageSubData = Get<PFNGLCOPYIMAGESUBDATAPROC>("glCopyImageSubData");
		fenceSync = Get<PFNGLFENCESYNCPROC>("glFenceSync");
		clientWaitSync = Get<PFNGLCLIENTWAITSYNCPROC>("glClientWaitSync");
		deleteSync = Get<PFNGLDELETESYNCPROC>("glDeleteSync");
		usable = getStringi && getUnsignedBytev && getUnsignedBytei && createMemoryObjects && deleteMemoryObjects && memoryObjectParameteriv && importMemoryFd
		    && texStorageMem2D && copyImageSubData && fenceSync && clientWaitSync && deleteSync;
		return usable;
	}
	// Whether the current context lists the extension (glXGetProcAddress finds functions whatever the context has)
	bool HasExtension(const char* name) const
	{
		GLint count = 0;
		glGetIntegerv(GL_NUM_EXTENSIONS, &count);
		for (GLint i = 0; i < count; i++) {
			const char* e = (const char*)getStringi(GL_EXTENSIONS, (GLuint)i);
			if (e && !strcmp(e, name))
				return true;
		}
		return false;
	}
};
#endif

} // namespace

struct OverlayTextureShare::Impl {
	VkInstance instance;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	uint32_t family;
	VkQueue queue;
	PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
	uint8_t uuid[VK_UUID_SIZE] = {}, driverUuid[VK_UUID_SIZE] = {};
	// Why each program device or context can't share (empty if it can), found at its first texture
	std::map<VkDevice, std::string> deviceVerdicts;
	bool usable = false;
	VkPhysicalDeviceMemoryProperties memory{};
	Commands session; // on the session's queue

	// The program's Vulkan device, as its last texture named it
	Commands program;
#ifdef SUPPORT_GL
	GlFunctions gl;
	std::map<GLXContext, std::string> contextVerdicts;
#endif

	// An image of the session's device shared with the program's side, for one overlay
	struct Shared {
		VkFormat format = VK_FORMAT_UNDEFINED;
		uint32_t width = 0, height = 0;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
		uint32_t memoryType = 0;
		bool dedicated = false;
		int fd = -1; // exported, kept to import again (each import takes a duplicate)
		bool acquired = false; // owned by the session's queue family (CopyIn), not yet handed back (Release)
		vr::VRVulkanTextureData_t data{};

		// The program's side: a Vulkan device's image, or an OpenGL texture
		VkDevice programDevice = VK_NULL_HANDLE;
		VkImage programImage = VK_NULL_HANDLE;
		VkDeviceMemory programMemory = VK_NULL_HANDLE;
#ifdef SUPPORT_GL
		GLuint glMemory = 0, glTexture = 0;
		GLXContext glContext = nullptr; // the context (and those sharing with it) the names are of
#endif
	};
	std::map<const void*, Shared> shared;

	void DropProgramSide(Shared& s)
	{
		if (s.programImage)
			vkDestroyImage(s.programDevice, s.programImage, nullptr);
		if (s.programMemory)
			vkFreeMemory(s.programDevice, s.programMemory, nullptr);
		s.programImage = VK_NULL_HANDLE;
		s.programMemory = VK_NULL_HANDLE;
		s.programDevice = VK_NULL_HANDLE;
#ifdef SUPPORT_GL
		// Only in their context; elsewhere the names may be another texture's, and are left
		if ((s.glTexture || s.glMemory) && s.glContext && glXGetCurrentContext() == s.glContext) {
			if (s.glTexture)
				glDeleteTextures(1, &s.glTexture);
			if (s.glMemory && gl.deleteMemoryObjects)
				gl.deleteMemoryObjects(1, &s.glMemory);
		}
		s.glTexture = 0;
		s.glMemory = 0;
		s.glContext = nullptr;
#endif
	}

	// Not while a copy on the session's queue may still use it: left, if it doesn't finish in time
	void DropWhenIdle(std::map<const void*, Shared>::iterator it)
	{
		if (session.busy && vkWaitForFences(device, 1, &session.fence, VK_TRUE, kWaitNs) != VK_SUCCESS) {
			OOVR_LOG("Overlay texture sharing: a copy still running; its shared image isn't freed");
			shared.erase(it);
			return;
		}
		session.busy = false;
		Drop(it->second);
		shared.erase(it);
	}

	void Drop(Shared& s)
	{
		DropProgramSide(s);
		if (s.image)
			vkDestroyImage(device, s.image, nullptr);
		if (s.memory)
			vkFreeMemory(device, s.memory, nullptr);
		if (s.fd >= 0)
			close(s.fd);
		s = Shared{};
	}

	VkImageCreateInfo ImageInfo(const Shared& s, const VkExternalMemoryImageCreateInfo* external) const
	{
		VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, external };
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = s.format;
		info.extent = { s.width, s.height, 1 };
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = kUsage;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return info;
	}

	// owner's image of this format and size, made (and exported) if it has none like it; nullptr, with why
	Shared* SharedFor(const void* owner, VkFormat format, uint32_t width, uint32_t height, std::string* why)
	{
		auto it = shared.find(owner);
		if (it != shared.end() && it->second.format == format && it->second.width == width && it->second.height == height)
			return &it->second;
		if (it != shared.end())
			DropWhenIdle(it);

		// Whether an image like it can be exported and imported as an opaque file descriptor, and how
		VkPhysicalDeviceExternalImageFormatInfo externalInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO };
		externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkPhysicalDeviceImageFormatInfo2 formatInfo{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalInfo };
		formatInfo.format = format;
		formatInfo.type = VK_IMAGE_TYPE_2D;
		formatInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		formatInfo.usage = kUsage;
		VkExternalImageFormatProperties externalProps{ VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
		VkImageFormatProperties2 formatProps{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalProps };
		if (vkGetPhysicalDeviceImageFormatProperties2(physicalDevice, &formatInfo, &formatProps) != VK_SUCCESS) {
			*why = "the session's device can't share an image of Vulkan format " + std::to_string((int)format);
			return nullptr;
		}
		const VkExternalMemoryFeatureFlags features = externalProps.externalMemoryProperties.externalMemoryFeatures;
		if (!(features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) || !(features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
			*why = "the session's device can't export and import an image of Vulkan format " + std::to_string((int)format);
			return nullptr;
		}

		Shared s;
		s.format = format;
		s.width = width;
		s.height = height;
		s.dedicated = (features & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
		VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		const VkImageCreateInfo info = ImageInfo(s, &external);
		bool ok = vkCreateImage(device, &info, nullptr, &s.image) == VK_SUCCESS;
		VkMemoryRequirements req{};
		if (ok)
			vkGetImageMemoryRequirements(device, s.image, &req);
		s.memoryType = UINT32_MAX;
		for (uint32_t i = 0; ok && i < memory.memoryTypeCount && s.memoryType == UINT32_MAX; i++)
			if ((req.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				s.memoryType = i;
		ok = ok && s.memoryType != UINT32_MAX;
		s.size = req.size;
		VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = s.image;
		VkExportMemoryAllocateInfo exportInfo{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, s.dedicated ? &dedicated : nullptr };
		exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo };
		alloc.allocationSize = s.size;
		alloc.memoryTypeIndex = s.memoryType;
		ok = ok && vkAllocateMemory(device, &alloc, nullptr, &s.memory) == VK_SUCCESS;
		ok = ok && vkBindImageMemory(device, s.image, s.memory, 0) == VK_SUCCESS;
		VkMemoryGetFdInfoKHR fdInfo{ VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
		fdInfo.memory = s.memory;
		fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		ok = ok && getMemoryFd(device, &fdInfo, &s.fd) == VK_SUCCESS;
		if (!ok) {
			*why = "no shared image of Vulkan format " + std::to_string((int)format) + " could be made";
			Drop(s);
			return nullptr;
		}
		s.data.m_nImage = (uint64_t)s.image;
		s.data.m_pDevice = device;
		s.data.m_pPhysicalDevice = physicalDevice;
		s.data.m_pInstance = instance;
		s.data.m_pQueue = queue;
		s.data.m_nQueueFamilyIndex = family;
		s.data.m_nWidth = width;
		s.data.m_nHeight = height;
		s.data.m_nFormat = (uint32_t)format;
		s.data.m_nSampleCount = 1;
		return &(shared[owner] = s);
	}

	// The image handed back to the program's side, after the session's copy from it (on the same queue)
	bool Release(Shared& s)
	{
		const bool ran = session.Run(queue, [&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier b = Barrier(s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_EXTERNAL);
			b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		});
		if (ran)
			s.acquired = false;
		return ran;
	}

	// The image taken over by the session's queue family from the program's side, which left it in layout
	bool Acquire(Shared& s, VkImageLayout layout)
	{
		const bool ran = session.Run(queue, [&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier b = Barrier(s.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, family);
			b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		});
		s.acquired = ran;
		return ran;
	}

	const vr::VRVulkanTextureData_t* CopyInVulkan(const vr::Texture_t* tex, const void* owner, std::string* why)
	{
		const auto* data = (const vr::VRVulkanTextureData_t*)tex->handle;
		if (!data || !data->m_nImage || !data->m_pDevice || !data->m_pPhysicalDevice || !data->m_pQueue || !data->m_nWidth || !data->m_nHeight) {
			*why = "the Vulkan texture's data is incomplete";
			return nullptr;
		}
		auto verdict = deviceVerdicts.find(data->m_pDevice);
		if (verdict == deviceVerdicts.end()) {
			std::string no;
			uint8_t device[VK_UUID_SIZE] = {}, driver[VK_UUID_SIZE] = {};
			// Only there if the program enabled VK_KHR_external_memory_fd on its device (where a driver answers anyway,
			// the import fails and the texture is read back)
			if (!vkGetDeviceProcAddr(data->m_pDevice, "vkGetMemoryFdKHR"))
				no = "the program's Vulkan device doesn't have VK_KHR_external_memory_fd";
			else if (!DeviceIds(data->m_pPhysicalDevice, device, driver) || memcmp(device, uuid, VK_UUID_SIZE) || memcmp(driver, driverUuid, VK_UUID_SIZE))
				no = "the program's Vulkan device is another GPU or driver than the session's";
			verdict = deviceVerdicts.emplace(data->m_pDevice, no).first;
		}
		if (!verdict->second.empty()) {
			*why = verdict->second;
			return nullptr;
		}
		if (program.device != data->m_pDevice || program.family != data->m_nQueueFamilyIndex || !program.fence) {
			// Another device: what was imported into the last one goes with it
			for (auto& kv : shared)
				if (kv.second.programDevice && kv.second.programDevice != data->m_pDevice)
					DropProgramSide(kv.second);
			if (!program.Make(data->m_pDevice, data->m_nQueueFamilyIndex)) {
				*why = "no command buffer for the program's queue family";
				return nullptr;
			}
		}
		Shared* s = SharedFor(owner, (VkFormat)data->m_nFormat, data->m_nWidth, data->m_nHeight, why);
		if (!s)
			return nullptr;
		if (s->acquired && !Release(*s)) {
			*why = "the shared image wasn't handed back after the last copy";
			return nullptr;
		}

		if (!s->programImage) {
			// The same image, made with the same parameters, on the program's device, in the memory it shares
			VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
			external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			const VkImageCreateInfo info = ImageInfo(*s, &external);
			s->programDevice = data->m_pDevice;
			bool ok = vkCreateImage(data->m_pDevice, &info, nullptr, &s->programImage) == VK_SUCCESS;
			VkMemoryRequirements req{};
			if (ok)
				vkGetImageMemoryRequirements(data->m_pDevice, s->programImage, &req);
			ok = ok && (req.memoryTypeBits & (1u << s->memoryType)) && req.size <= s->size;
			const int fd = ok ? dup(s->fd) : -1;
			VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
			dedicated.image = s->programImage;
			VkImportMemoryFdInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, s->dedicated ? &dedicated : nullptr };
			import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
			import.fd = fd;
			VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
			alloc.allocationSize = s->size;
			alloc.memoryTypeIndex = s->memoryType;
			const bool imported = ok && fd >= 0 && vkAllocateMemory(data->m_pDevice, &alloc, nullptr, &s->programMemory) == VK_SUCCESS;
			// A successful import owns the descriptor; a failed one leaves it to be closed
			if (!imported && fd >= 0)
				close(fd);
			ok = imported && vkBindImageMemory(data->m_pDevice, s->programImage, s->programMemory, 0) == VK_SUCCESS;
			if (!ok) {
				*why = "the program's Vulkan device couldn't import the shared image";
				DropProgramSide(*s);
				return nullptr;
			}
		}

		// On the program's queue, during its call: its image (TRANSFER_SRC_OPTIMAL) into the shared one, which is then
		// handed to the session's device
		const uint32_t programFamily = data->m_nQueueFamilyIndex;
		const bool ran = program.Run((VkQueue)data->m_pQueue, [&](VkCommandBuffer cmd) {
			VkImageMemoryBarrier toDst = Barrier(s->programImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
			    VK_QUEUE_FAMILY_IGNORED);
			toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
			VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			if (data->m_nSampleCount > 1) {
				VkImageResolve region{ layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { s->width, s->height, 1 } };
				vkCmdResolveImage(cmd, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->programImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			} else {
				VkImageCopy region{ layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { s->width, s->height, 1 } };
				vkCmdCopyImage(cmd, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->programImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			}
			VkImageMemoryBarrier release = Barrier(s->programImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, programFamily, VK_QUEUE_FAMILY_EXTERNAL);
			release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
		});
		if (!ran) {
			*why = "the copy on the program's queue didn't finish in time";
			return nullptr;
		}
		if (!Acquire(*s, VK_IMAGE_LAYOUT_GENERAL)) {
			*why = "the session's queue didn't take the shared image in time";
			return nullptr;
		}
		return &s->data;
	}

#ifdef SUPPORT_GL
	const vr::VRVulkanTextureData_t* CopyInGl(const vr::Texture_t* tex, const void* owner, std::string* why)
	{
		const GLuint name = (GLuint)(uintptr_t)tex->handle;
		const GLXContext context = glXGetCurrentContext();
		if (!name || !context) {
			*why = "no OpenGL texture, or no GLX context current";
			return nullptr;
		}
		auto verdict = contextVerdicts.find(context);
		if (verdict == contextVerdicts.end()) {
			std::string no;
			if (!gl.Load() || !gl.HasExtension("GL_EXT_memory_object_fd")) {
				no = "the program's OpenGL context doesn't have GL_EXT_memory_object_fd";
			} else {
				// The session's driver, and one of the context's devices
				GLubyte driver[GL_UUID_SIZE_EXT] = {};
				gl.getUnsignedBytev(GL_DRIVER_UUID_EXT, driver);
				GLint devices = 0;
				glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &devices);
				bool same = false;
				for (GLint i = 0; i < devices && !same; i++) {
					GLubyte id[GL_UUID_SIZE_EXT] = {};
					gl.getUnsignedBytei(GL_DEVICE_UUID_EXT, (GLuint)i, id);
					same = memcmp(id, uuid, VK_UUID_SIZE) == 0;
				}
				if (!same || memcmp(driver, driverUuid, VK_UUID_SIZE))
					no = "the program's OpenGL context is on another GPU or driver than the session's";
			}
			verdict = contextVerdicts.emplace(context, no).first;
		}
		if (!verdict->second.empty()) {
			*why = verdict->second;
			return nullptr;
		}

		while (glGetError() != GL_NO_ERROR) {
		}
		GLint previous = 0, width = 0, height = 0, internalFormat = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		glBindTexture(GL_TEXTURE_2D, name);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
		glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
		const VkFormat format = VkFormatOfGl(internalFormat);
		if (glGetError() != GL_NO_ERROR || width <= 0 || height <= 0 || format == VK_FORMAT_UNDEFINED) {
			*why = "an OpenGL texture of internal format " + std::to_string(internalFormat) + " (or not a 2D texture) can't be shared";
			return nullptr;
		}
		Shared* s = SharedFor(owner, format, (uint32_t)width, (uint32_t)height, why);
		if (!s)
			return nullptr;
		if (s->acquired && !Release(*s)) {
			*why = "the shared image wasn't handed back after the last copy";
			return nullptr;
		}

		// Names made in another context aren't this one's to use
		if (s->glTexture && s->glContext != context) {
			s->glTexture = 0;
			s->glMemory = 0;
		}
		if (!s->glTexture) {
			s->glContext = context;
			// The shared memory as a texture of the program's context, laid out as the image is (optimal tiling)
			const int fd = dup(s->fd);
			gl.createMemoryObjects(1, &s->glMemory);
			if (s->dedicated) {
				const GLint yes = GL_TRUE;
				gl.memoryObjectParameteriv(s->glMemory, GL_DEDICATED_MEMORY_OBJECT_EXT, &yes);
			}
			// A successful import owns the descriptor
			bool imported = false;
			if (fd >= 0) {
				gl.importMemoryFd(s->glMemory, (GLuint64)s->size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
				imported = glGetError() == GL_NO_ERROR;
				if (!imported)
					close(fd);
			}
			glGenTextures(1, &s->glTexture);
			glBindTexture(GL_TEXTURE_2D, s->glTexture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
			gl.texStorageMem2D(GL_TEXTURE_2D, 1, (GLenum)internalFormat, width, height, s->glMemory, 0);
			glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
			if (!imported || glGetError() != GL_NO_ERROR) {
				// Not tried again in this context
				*why = "the program's OpenGL context couldn't import the shared image";
				contextVerdicts[context] = *why;
				DropProgramSide(*s);
				return nullptr;
			}
		}

		// In the program's context, during its call: its texture into the shared one, waited for
		gl.copyImageSubData(name, GL_TEXTURE_2D, 0, 0, 0, 0, s->glTexture, GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);
		GLsync sync = gl.fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		const GLenum waited = sync ? gl.clientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, kWaitNs) : GL_WAIT_FAILED;
		if (sync)
			gl.deleteSync(sync);
		if (glGetError() != GL_NO_ERROR || (waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED)) {
			*why = "the copy in the program's OpenGL context failed or didn't finish in time";
			return nullptr;
		}
		// OpenGL says nothing of the image's layout without GL_EXT_semaphore: taken as GENERAL
		if (!Acquire(*s, VK_IMAGE_LAYOUT_GENERAL)) {
			*why = "the session's queue didn't take the shared image in time";
			return nullptr;
		}
		return &s->data;
	}
#endif
};

OverlayTextureShare::OverlayTextureShare(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t queueFamily, VkQueue queue)
    : impl(std::make_unique<Impl>())
{
	Impl& i = *impl;
	i.instance = instance;
	i.physicalDevice = physicalDevice;
	i.device = device;
	i.family = queueFamily;
	i.queue = queue;
	i.getMemoryFd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &i.memory);
	const bool hasUuid = DeviceIds(physicalDevice, i.uuid, i.driverUuid);
	i.usable = i.getMemoryFd && hasUuid && i.session.Make(device, queueFamily);
	if (!i.usable)
		OOVR_LOGF("Overlay texture sharing: the session's Vulkan device can't share images (vkGetMemoryFdKHR %s, device UUID %s); overlay "
		          "textures are read back",
		    i.getMemoryFd ? "found" : "missing", hasUuid ? "read" : "unknown");
}

OverlayTextureShare::~OverlayTextureShare()
{
	// What's of the program's device or context is left: the program may have destroyed them by now
	for (auto& kv : impl->shared) {
		kv.second.programImage = VK_NULL_HANDLE;
		kv.second.programMemory = VK_NULL_HANDLE;
#ifdef SUPPORT_GL
		kv.second.glContext = nullptr;
#endif
		impl->Drop(kv.second);
	}
	impl->session.Drop();
}

const vr::VRVulkanTextureData_t* OverlayTextureShare::CopyIn(const vr::Texture_t* tex, const void* owner, bool* flipY, std::string* why)
{
	*flipY = false;
	if (!impl->usable) {
		*why = "the session's Vulkan device can't share images";
		return nullptr;
	}
	if (tex->eType == vr::TextureType_Vulkan)
		return impl->CopyInVulkan(tex, owner, why);
#ifdef SUPPORT_GL
	if (tex->eType == vr::TextureType_OpenGL) {
		*flipY = true;
		return impl->CopyInGl(tex, owner, why);
	}
#endif
	*why = "only Vulkan and OpenGL textures are shared";
	return nullptr;
}

void OverlayTextureShare::Release(const void* owner)
{
	auto it = impl->shared.find(owner);
	if (it == impl->shared.end() || !it->second.acquired)
		return;
	if (!impl->Release(it->second))
		OOVR_LOG_ONCE("Overlay texture sharing: the session's copy from a shared image didn't finish in time");
}

void OverlayTextureShare::Forget(const void* owner)
{
	auto it = impl->shared.find(owner);
	if (it != impl->shared.end())
		impl->DropWhenIdle(it);
}

#endif
