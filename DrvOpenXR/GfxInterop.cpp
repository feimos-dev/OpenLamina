#include "GfxInterop.h"

#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef SUPPORT_GL
#include <GL/gl.h>
#endif

#ifdef SUPPORT_VK
#include <vulkan/vulkan_win32.h>
#endif

#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/logging.h"

#include <chrono>
#include <map>

using Microsoft::WRL::ComPtr;

namespace {

// More textures than a game submits in turn (its swapchain's, a few per eye): the copies are dropped and made again
constexpr size_t kMaxCopies = 16;

uint64_t PackLuid(const LUID& luid)
{
	return (uint64_t)luid.LowPart | ((uint64_t)(uint32_t)luid.HighPart << 32);
}

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT miscFlags)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = miscFlags;
	ComPtr<ID3D11Texture2D> tex;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex)))
		return nullptr;
	return tex;
}

} // namespace

GfxInterop::~GfxInterop()
{
	if (device)
		device->Release();
}

bool GfxInterop::MakeDevice()
{
	uint64_t luid = 0;
	if (!BaseSystem::_RuntimeAdapterLuid(&luid)) {
		OOVR_LOG("Graphics interop: the OpenXR runtime names no D3D11 adapter");
		return false;
	}
	ComPtr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return false;
	ComPtr<IDXGIAdapter1> adapter;
	ComPtr<IDXGIAdapter1> candidate;
	// Bounded by the adapter count (EnumAdapters1 ends with DXGI_ERROR_NOT_FOUND)
	for (UINT i = 0; !adapter && factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(candidate->GetDesc1(&desc)) && PackLuid(desc.AdapterLuid) == luid)
			adapter = candidate;
		candidate.Reset();
	}
	if (!adapter) {
		OOVR_LOG("Graphics interop: no DXGI adapter has the OpenXR runtime's LUID");
		return false;
	}
	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
	if (hr == E_INVALIDARG) // no 11.1 on this system
		hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION, &device, nullptr, nullptr);
	if (FAILED(hr)) {
		OOVR_LOGF("Graphics interop: no D3D11 device on the runtime's adapter: 0x%08lx", (unsigned long)hr);
		device = nullptr;
		return false;
	}
	return true;
}

// --- OpenGL, through WGL_NV_DX_interop2 ---

#ifdef SUPPORT_GL

namespace {

constexpr GLenum kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9;
constexpr GLenum kReadFramebufferBinding = 0x8CAA, kDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kColorAttachment0 = 0x8CE0, kFramebufferSrgb = 0x8DB9;
constexpr GLenum kAccessWriteDiscard = 0x0002; // WGL_ACCESS_WRITE_DISCARD_NV

// The DXGI format of a GL internal format (the pattern's and the common colour formats), DXGI_FORMAT_UNKNOWN else
DXGI_FORMAT DxgiOfGl(GLint format)
{
	switch (format) {
	case 0x8C43: // GL_SRGB8_ALPHA8
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 0x8058: // GL_RGBA8
	case GL_RGBA:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case 0x881A: // GL_RGBA16F
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case 0x8059: // GL_RGB10_A2
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

class GLInterop : public GfxInterop {
public:
	using OpenDevice = HANDLE(WINAPI*)(void*);
	using CloseDevice = BOOL(WINAPI*)(HANDLE);
	using RegisterObject = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
	using UnregisterObject = BOOL(WINAPI*)(HANDLE, HANDLE);
	using LockObjects = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
	using GenFramebuffers = void(APIENTRY*)(GLsizei, GLuint*);
	using DeleteFramebuffers = void(APIENTRY*)(GLsizei, const GLuint*);
	using BindFramebuffer = void(APIENTRY*)(GLenum, GLuint);
	using FramebufferTexture2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
	using BlitFramebuffer = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);

	OpenDevice openDevice = nullptr;
	CloseDevice closeDevice = nullptr;
	RegisterObject registerObject = nullptr;
	UnregisterObject unregisterObject = nullptr;
	LockObjects lockObjects = nullptr, unlockObjects = nullptr;
	GenFramebuffers genFramebuffers = nullptr;
	DeleteFramebuffers deleteFramebuffers = nullptr;
	BindFramebuffer bindFramebuffer = nullptr;
	FramebufferTexture2D framebufferTexture2D = nullptr;
	BlitFramebuffer blitFramebuffer = nullptr;

	HANDLE interopDevice = nullptr;
	GLuint framebuffers[2] = {};

	struct Copy {
		GLint width = 0, height = 0, format = 0;
		ComPtr<ID3D11Texture2D> tex;
		GLuint name = 0;
		HANDLE object = nullptr;
	};
	std::map<GLuint, Copy> copies;

	vr::ETextureType Type() const override { return vr::TextureType_OpenGL; }

	bool Init()
	{
#define OC_GL_LOAD(var, name)                              \
	if (!(var = (decltype(var))wglGetProcAddress(name))) { \
		OOVR_LOG("Graphics interop: OpenGL lacks " name);  \
		return false;                                      \
	}
		OC_GL_LOAD(openDevice, "wglDXOpenDeviceNV");
		OC_GL_LOAD(closeDevice, "wglDXCloseDeviceNV");
		OC_GL_LOAD(registerObject, "wglDXRegisterObjectNV");
		OC_GL_LOAD(unregisterObject, "wglDXUnregisterObjectNV");
		OC_GL_LOAD(lockObjects, "wglDXLockObjectsNV");
		OC_GL_LOAD(unlockObjects, "wglDXUnlockObjectsNV");
		OC_GL_LOAD(genFramebuffers, "glGenFramebuffers");
		OC_GL_LOAD(deleteFramebuffers, "glDeleteFramebuffers");
		OC_GL_LOAD(bindFramebuffer, "glBindFramebuffer");
		OC_GL_LOAD(framebufferTexture2D, "glFramebufferTexture2D");
		OC_GL_LOAD(blitFramebuffer, "glBlitFramebuffer");
#undef OC_GL_LOAD
		if (!MakeDevice())
			return false;
		interopDevice = openDevice(device);
		if (!interopDevice) {
			OOVR_LOGF("Graphics interop: wglDXOpenDeviceNV failed (%lu)", GetLastError());
			return false;
		}
		genFramebuffers(2, framebuffers);
		return true;
	}

	void Drop(Copy& copy)
	{
		if (copy.object)
			unregisterObject(interopDevice, copy.object);
		if (copy.name)
			glDeleteTextures(1, &copy.name);
	}

	~GLInterop() override
	{
		// The GL objects need the game's context, current on the thread that ends the session if it's the game's own
		// render thread; without one they're left (the context goes with the game)
		if (wglGetCurrentContext() && interopDevice) {
			for (auto& kv : copies)
				Drop(kv.second);
			if (framebuffers[0])
				deleteFramebuffers(2, framebuffers);
			closeDevice(interopDevice);
		}
	}

	Copy* CopyFor(GLuint src)
	{
		GLint previous = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
		glBindTexture(GL_TEXTURE_2D, src);
		GLint width = 0, height = 0, format = 0;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
		glBindTexture(GL_TEXTURE_2D, previous);

		auto it = copies.find(src);
		if (it != copies.end() && it->second.width == width && it->second.height == height && it->second.format == format)
			return &it->second;
		if (it != copies.end()) {
			Drop(it->second);
			copies.erase(it);
		}
		if (copies.size() >= kMaxCopies) {
			for (auto& kv : copies)
				Drop(kv.second);
			copies.clear();
		}

		DXGI_FORMAT dxgi = DxgiOfGl(format);
		if (dxgi == DXGI_FORMAT_UNKNOWN || width <= 0 || height <= 0) {
			OOVR_LOG_ONCEF("Graphics interop: an OpenGL texture of internal format 0x%x (%dx%d) can't be copied", format, width, height);
			return nullptr;
		}
		Copy copy;
		copy.width = width;
		copy.height = height;
		copy.format = format;
		copy.tex = MakeTexture(device, width, height, dxgi, 0);
		if (copy.tex) {
			glGenTextures(1, &copy.name);
			copy.object = registerObject(interopDevice, copy.tex.Get(), copy.name, GL_TEXTURE_2D, kAccessWriteDiscard);
		}
		if (!copy.object) {
			OOVR_LOG_ONCEF("Graphics interop: an OpenGL texture of internal format 0x%x couldn't be shared with D3D11", format);
			Drop(copy);
			return nullptr;
		}
		return &(copies[src] = std::move(copy));
	}

	ID3D11Texture2D* CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t*) override
	{
		GLuint src = (GLuint)(intptr_t)tex->handle;
		Copy* copy = CopyFor(src);
		if (!copy)
			return nullptr;
		if (!lockObjects(interopDevice, 1, &copy->object)) {
			OOVR_LOG_ONCE("Graphics interop: wglDXLockObjectsNV failed: frame left out");
			return nullptr;
		}
		// The game's state is put back afterwards: its framebuffers, scissor and sRGB conversion (a blit with it on
		// would convert the texels)
		GLint read = 0, draw = 0;
		glGetIntegerv(kReadFramebufferBinding, &read);
		glGetIntegerv(kDrawFramebufferBinding, &draw);
		GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), srgb = glIsEnabled(kFramebufferSrgb);
		glDisable(GL_SCISSOR_TEST);
		glDisable(kFramebufferSrgb);
		bindFramebuffer(kReadFramebuffer, framebuffers[0]);
		framebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, src, 0);
		bindFramebuffer(kDrawFramebuffer, framebuffers[1]);
		framebufferTexture2D(kDrawFramebuffer, kColorAttachment0, GL_TEXTURE_2D, copy->name, 0);
		// GL's rows run bottom-up, D3D's top-down: turned over as it's copied
		blitFramebuffer(0, 0, copy->width, copy->height, 0, copy->height, copy->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		framebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, 0, 0);
		framebufferTexture2D(kDrawFramebuffer, kColorAttachment0, GL_TEXTURE_2D, 0, 0);
		bindFramebuffer(kReadFramebuffer, read);
		bindFramebuffer(kDrawFramebuffer, draw);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);
		if (srgb)
			glEnable(kFramebufferSrgb);
		GLenum error = glGetError();
		unlockObjects(interopDevice, 1, &copy->object);
		if (error != GL_NO_ERROR) {
			OOVR_LOG_ONCEF("Graphics interop: the OpenGL copy failed (0x%x): frame left out", error);
			return nullptr;
		}
		return copy->tex.Get();
	}
};

} // namespace

std::unique_ptr<GfxInterop> GfxInterop::ForGL()
{
	if (!wglGetCurrentContext()) {
		OOVR_LOG("Graphics interop: no current OpenGL context");
		return nullptr;
	}
	auto interop = std::make_unique<GLInterop>();
	if (!interop->Init())
		return nullptr;
	OOVR_LOG("Graphics interop: OpenGL frames go through Direct3D 11");
	return interop;
}

#else

std::unique_ptr<GfxInterop> GfxInterop::ForGL()
{
	return nullptr;
}

#endif

// --- Vulkan, importing D3D11 textures through VK_KHR_external_memory_win32 ---

#ifdef SUPPORT_VK

namespace {

DXGI_FORMAT DxgiOfVk(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case VK_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case VK_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

// The typeless one of a format's group, for a shared texture where the typed one can't be shared
DXGI_FORMAT TypelessOf(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	default:
		return format;
	}
}

// How long a copy may take on the game's queue: far more than one does, so a longer one is a stuck GPU
constexpr uint64_t kCopyTimeoutNs = 100'000'000;

class VkInterop : public GfxInterop {
public:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer commands = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	PFN_vkGetMemoryWin32HandlePropertiesKHR getHandleProperties = nullptr;

	struct Copy {
		uint32_t width = 0, height = 0;
		VkFormat format = VK_FORMAT_UNDEFINED;
		ComPtr<ID3D11Texture2D> tex;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
	};
	std::map<VkImage, Copy> copies;

	vr::ETextureType Type() const override { return vr::TextureType_Vulkan; }

	void Drop(Copy& copy)
	{
		if (copy.image)
			vkDestroyImage(vkDevice, copy.image, nullptr);
		if (copy.memory)
			vkFreeMemory(vkDevice, copy.memory, nullptr);
	}

	~VkInterop() override
	{
		if (fence)
			vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, kCopyTimeoutNs);
		for (auto& kv : copies)
			Drop(kv.second);
		if (fence)
			vkDestroyFence(vkDevice, fence, nullptr);
		if (pool)
			vkDestroyCommandPool(vkDevice, pool, nullptr);
	}

	bool Init()
	{
		// Only there if the game enabled VK_KHR_external_memory_win32 on its device
		getHandleProperties = (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(vkDevice, "vkGetMemoryWin32HandlePropertiesKHR");
		if (!getHandleProperties) {
			OOVR_LOG("Graphics interop: the game's Vulkan device lacks VK_KHR_external_memory_win32");
			return false;
		}
		if (!MakeDevice())
			return false;

		// The game's GPU must be the runtime's
		VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id };
		vkGetPhysicalDeviceProperties2(physicalDevice, &props);
		uint64_t runtimeLuid = 0, gameLuid = 0;
		BaseSystem::_RuntimeAdapterLuid(&runtimeLuid);
		memcpy(&gameLuid, id.deviceLUID, sizeof(gameLuid));
		if (!id.deviceLUIDValid || gameLuid != runtimeLuid) {
			OOVR_LOG("Graphics interop: the game's Vulkan device isn't on the OpenXR runtime's adapter");
			return false;
		}

		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		return vkCreateFence(vkDevice, &fenceInfo, nullptr, &fence) == VK_SUCCESS;
	}

	// A command buffer of the game's queue's family (which comes with each texture)
	bool CommandsFor(uint32_t queueFamily)
	{
		if (pool && family == queueFamily)
			return true;
		if (pool)
			vkDestroyCommandPool(vkDevice, pool, nullptr);
		pool = VK_NULL_HANDLE;
		commands = VK_NULL_HANDLE;
		family = queueFamily;
		VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = family;
		if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
			pool = VK_NULL_HANDLE;
			return false;
		}
		VkCommandBufferAllocateInfo bufferInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		bufferInfo.commandPool = pool;
		bufferInfo.commandBufferCount = 1;
		bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		return vkAllocateCommandBuffers(vkDevice, &bufferInfo, &commands) == VK_SUCCESS;
	}

	Copy* CopyFor(const vr::VRVulkanTextureData_t* data)
	{
		VkImage src = (VkImage)data->m_nImage;
		auto it = copies.find(src);
		if (it != copies.end() && it->second.width == data->m_nWidth && it->second.height == data->m_nHeight
		    && it->second.format == (VkFormat)data->m_nFormat)
			return &it->second;
		if (it != copies.end()) {
			Drop(it->second);
			copies.erase(it);
		}
		if (copies.size() >= kMaxCopies) {
			for (auto& kv : copies)
				Drop(kv.second);
			copies.clear();
		}

		DXGI_FORMAT dxgi = DxgiOfVk((VkFormat)data->m_nFormat);
		if (dxgi == DXGI_FORMAT_UNKNOWN || !data->m_nWidth || !data->m_nHeight) {
			OOVR_LOG_ONCEF("Graphics interop: a Vulkan texture of format %u (%ux%u) can't be copied", data->m_nFormat, data->m_nWidth, data->m_nHeight);
			return nullptr;
		}

		Copy copy;
		copy.width = data->m_nWidth;
		copy.height = data->m_nHeight;
		copy.format = (VkFormat)data->m_nFormat;
		const UINT shared = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		copy.tex = MakeTexture(device, copy.width, copy.height, dxgi, shared);
		if (!copy.tex)
			copy.tex = MakeTexture(device, copy.width, copy.height, TypelessOf(dxgi), shared);
		ComPtr<IDXGIResource1> resource;
		HANDLE handle = nullptr;
		if (!copy.tex || FAILED(copy.tex.As(&resource))
		    || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
			OOVR_LOG_ONCEF("Graphics interop: no shared D3D11 texture of DXGI format %d", (int)dxgi);
			return nullptr;
		}

		VkExternalMemoryImageCreateInfo external{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
		VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external };
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = copy.format;
		imageInfo.extent = { copy.width, copy.height, 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bool ok = vkCreateImage(vkDevice, &imageInfo, nullptr, &copy.image) == VK_SUCCESS;

		VkMemoryWin32HandlePropertiesKHR handleProps{ VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
		ok = ok && getHandleProperties(vkDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle, &handleProps) == VK_SUCCESS;
		VkMemoryRequirements requirements{};
		if (ok)
			vkGetImageMemoryRequirements(vkDevice, copy.image, &requirements);
		uint32_t bits = requirements.memoryTypeBits & handleProps.memoryTypeBits;
		uint32_t type = 0;
		while (type < 32 && !(bits & (1u << type)))
			type++;
		ok = ok && type < 32;

		VkMemoryDedicatedAllocateInfo dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
		dedicated.image = copy.image;
		VkImportMemoryWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated };
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
		import.handle = handle;
		VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import };
		allocInfo.allocationSize = requirements.size;
		allocInfo.memoryTypeIndex = type;
		ok = ok && vkAllocateMemory(vkDevice, &allocInfo, nullptr, &copy.memory) == VK_SUCCESS;
		ok = ok && vkBindImageMemory(vkDevice, copy.image, copy.memory, 0) == VK_SUCCESS;
		// Importing doesn't take the handle over
		CloseHandle(handle);
		if (!ok) {
			OOVR_LOG_ONCEF("Graphics interop: the shared D3D11 texture couldn't be imported into the game's Vulkan device (format %u)", data->m_nFormat);
			Drop(copy);
			return nullptr;
		}
		return &(copies[src] = std::move(copy));
	}

	ID3D11Texture2D* CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t*) override
	{
		auto* data = (const vr::VRVulkanTextureData_t*)tex->handle;
		if (!CommandsFor(data->m_nQueueFamilyIndex)) {
			OOVR_LOG_ONCE("Graphics interop: no Vulkan command buffer for the game's queue family");
			return nullptr;
		}
		Copy* copy = CopyFor(data);
		if (!copy)
			return nullptr;

		vkResetCommandBuffer(commands, 0);
		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(commands, &begin);

		// The game's image is in TRANSFER_SRC_OPTIMAL, as OpenVR asks of Vulkan textures; the copy is made ready to be
		// written, and afterwards handed to D3D11
		VkImageMemoryBarrier toDst{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toDst.srcAccessMask = 0;
		toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = copy->image;
		toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

		VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		if (data->m_nSampleCount > 1) {
			VkImageResolve region{ layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { copy->width, copy->height, 1 } };
			vkCmdResolveImage(commands, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		} else {
			VkImageCopy region{ layers, { 0, 0, 0 }, layers, { 0, 0, 0 }, { copy->width, copy->height, 1 } };
			vkCmdCopyImage(commands, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		VkImageMemoryBarrier release = toDst;
		release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		release.dstAccessMask = 0;
		release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		release.srcQueueFamilyIndex = family;
		release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
		vkEndCommandBuffer(commands);

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		vkResetFences(vkDevice, 1, &fence);
		// On the queue that comes with the texture, the game's own
		VkQueue target = data->m_pQueue ? (VkQueue)data->m_pQueue : queue;
		if (vkQueueSubmit(target, 1, &submit, fence) != VK_SUCCESS) {
			OOVR_LOG_ONCE("Graphics interop: vkQueueSubmit failed: frame left out");
			return nullptr;
		}
		// Done before D3D11 reads it
		if (vkWaitForFences(vkDevice, 1, &fence, VK_TRUE, kCopyTimeoutNs) != VK_SUCCESS) {
			OOVR_LOG_ONCE("Graphics interop: the Vulkan copy didn't finish in time: frame left out");
			return nullptr;
		}
		return copy->tex.Get();
	}
};

} // namespace

std::unique_ptr<GfxInterop> GfxInterop::ForVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue)
{
	if (!instance || !physicalDevice || !device || !queue)
		return nullptr;
	auto interop = std::make_unique<VkInterop>();
	interop->instance = instance;
	interop->physicalDevice = physicalDevice;
	interop->vkDevice = device;
	interop->queue = queue;
	if (!interop->Init())
		return nullptr;
	OOVR_LOG("Graphics interop: Vulkan frames go through Direct3D 11");
	return interop;
}

#endif

#else // not Windows with D3D11

GfxInterop::~GfxInterop() = default;

std::unique_ptr<GfxInterop> GfxInterop::ForGL()
{
	return nullptr;
}

#ifdef SUPPORT_VK
std::unique_ptr<GfxInterop> GfxInterop::ForVulkan(VkInstance, VkPhysicalDevice, VkDevice, VkQueue)
{
	return nullptr;
}
#endif

bool GfxInterop::MakeDevice()
{
	return false;
}

#endif
