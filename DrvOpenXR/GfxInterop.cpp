#include "GfxInterop.h"

#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#ifdef SUPPORT_GL
#include <GL/gl.h>
#endif

#ifdef SUPPORT_VK
#include <vulkan/vulkan_win32.h>
#endif

#include "../OpenOVR/Compositor/FormatTable.h"
#include "../OpenOVR/Compositor/compositor.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/logging.h"

#include <chrono>
#include <map>
#include <optional>
#include <utility>

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

void GfxInterop::UseDevice(ID3D11Device* onDevice)
{
	if (!onDevice)
		return;
	onDevice->AddRef();
	if (device)
		device->Release();
	device = onDevice;
}

bool GfxInterop::MakeDevice()
{
	if (device)
		return true; // given
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

constexpr GLenum kTexture2DArray = 0x8C1A, kTextureBinding2DArray = 0x8C1D, kTextureDepth = 0x8071;
constexpr GLenum kTexture2DMultisample = 0x9100, kTextureBinding2DMultisample = 0x9104;
constexpr GLenum kRenderbuffer = 0x8D41, kRenderbufferBinding = 0x8CA7, kRenderbufferWidth = 0x8D42, kRenderbufferHeight = 0x8D43,
                 kRenderbufferInternalFormat = 0x8D44;

// The DXGI format of a GL internal format: the one of the same texels, else one the blit converts into; UNKNOWN for
// formats no table knows (and compressed or integer ones)
DXGI_FORMAT DxgiOfGl(GLint format)
{
	std::optional<int64_t> dxgi = SameTexelsIn(GfxApiKind::OpenGL, format, GfxApiKind::D3D11);
	if (!dxgi)
		dxgi = ConvertibleColourIn(GfxApiKind::OpenGL, format, GfxApiKind::D3D11);
	return dxgi ? (DXGI_FORMAT)*dxgi : DXGI_FORMAT_UNKNOWN;
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
	using FramebufferTextureLayer = void(APIENTRY*)(GLenum, GLenum, GLuint, GLint, GLint);
	using FramebufferRenderbuffer = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
	using BindRenderbuffer = void(APIENTRY*)(GLenum, GLuint);
	using GetRenderbufferParameteriv = void(APIENTRY*)(GLenum, GLenum, GLint*);

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
	// Optional: without them, array textures and renderbuffers aren't copied
	FramebufferTextureLayer framebufferTextureLayer = nullptr;
	FramebufferRenderbuffer framebufferRenderbuffer = nullptr;
	BindRenderbuffer bindRenderbuffer = nullptr;
	GetRenderbufferParameteriv getRenderbufferParameteriv = nullptr;

	HANDLE interopDevice = nullptr;
	GLuint framebuffers[2] = {};

	// How the game's image is read: a texture's level 0, a slice of an array texture, a renderbuffer or a multisampled
	// texture (the last two through a resolving blit)
	enum class Kind {
		Texture,
		Array,
		Renderbuffer,
		Multisampled,
	};
	struct Source {
		Kind kind = Kind::Texture;
		GLint layer = 0;
		GLint width = 0, height = 0, format = 0, layers = 1;
	};

	struct Copy {
		GLint width = 0, height = 0, format = 0;
		ComPtr<ID3D11Texture2D> tex;
		GLuint name = 0;
		HANDLE object = nullptr;
	};
	// By name and kind (a renderbuffer and a texture may have the same name)
	std::map<std::pair<GLuint, Kind>, Copy> copies;

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
		framebufferTextureLayer = (FramebufferTextureLayer)wglGetProcAddress("glFramebufferTextureLayer");
		framebufferRenderbuffer = (FramebufferRenderbuffer)wglGetProcAddress("glFramebufferRenderbuffer");
		bindRenderbuffer = (BindRenderbuffer)wglGetProcAddress("glBindRenderbuffer");
		getRenderbufferParameteriv = (GetRenderbufferParameteriv)wglGetProcAddress("glGetRenderbufferParameteriv");
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

	// Reads the image's size and format as `kind` (a texture's target bound for the query, and bound back after)
	void Query(GLuint src, Source& source)
	{
		source.width = source.height = source.format = 0;
		source.layers = 1;
		if (source.kind == Kind::Renderbuffer) {
			GLint previous = 0;
			glGetIntegerv(kRenderbufferBinding, &previous);
			bindRenderbuffer(kRenderbuffer, src);
			getRenderbufferParameteriv(kRenderbuffer, kRenderbufferWidth, &source.width);
			getRenderbufferParameteriv(kRenderbuffer, kRenderbufferHeight, &source.height);
			getRenderbufferParameteriv(kRenderbuffer, kRenderbufferInternalFormat, &source.format);
			bindRenderbuffer(kRenderbuffer, (GLuint)previous);
			return;
		}
		GLenum target = GL_TEXTURE_2D, binding = GL_TEXTURE_BINDING_2D;
		if (source.kind == Kind::Array) {
			target = kTexture2DArray;
			binding = kTextureBinding2DArray;
		} else if (source.kind == Kind::Multisampled) {
			target = kTexture2DMultisample;
			binding = kTextureBinding2DMultisample;
		}
		GLint previous = 0;
		glGetIntegerv(binding, &previous);
		glBindTexture(target, src);
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_WIDTH, &source.width);
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_HEIGHT, &source.height);
		glGetTexLevelParameteriv(target, 0, GL_TEXTURE_INTERNAL_FORMAT, &source.format);
		if (source.kind == Kind::Array)
			glGetTexLevelParameteriv(target, 0, kTextureDepth, &source.layers);
		glBindTexture(target, (GLuint)previous);
	}

	// What the submit names: the slice as GLCompositor picks it (SubmitWithArrayIndex's, else the eye's for an array
	// texture), a renderbuffer, or a texture, multisampled if it has no plain level 0. False (logged) if it's none of
	// them, or this GL lacks what's needed to read it. The GL errors of the tries are the caller's to clear.
	bool Identify(GLuint src, vr::EVRSubmitFlags submitFlags, int eye, Source& source)
	{
		const std::optional<uint32_t> slice = SubmitArraySlice::Current();
		const bool array = (submitFlags & vr::Submit_GlArrayTexture) || slice.has_value();
		source.layer = array ? (GLint)(slice ? *slice : (uint32_t)(eye == 1 ? 1 : 0)) : 0;
		if ((submitFlags & vr::Submit_GlRenderBuffer) && !array) {
			if (!bindRenderbuffer || !getRenderbufferParameteriv || !framebufferRenderbuffer) {
				OOVR_LOG_ONCE("Graphics interop: this OpenGL can't read renderbuffers: frame left out");
				return false;
			}
			source.kind = Kind::Renderbuffer;
			Query(src, source);
		} else if (array) {
			if (!framebufferTextureLayer) {
				OOVR_LOG_ONCE("Graphics interop: this OpenGL can't read array textures: frame left out");
				return false;
			}
			source.kind = Kind::Array;
			Query(src, source);
			// A plain texture given to SubmitWithArrayIndex with slice 0 is taken as it is
			if (source.width <= 0 && source.layer == 0) {
				source.kind = Kind::Texture;
				Query(src, source);
			}
		} else {
			source.kind = Kind::Texture;
			Query(src, source);
			if (source.width <= 0) {
				source.kind = Kind::Multisampled;
				Query(src, source);
			}
		}
		if (source.width <= 0 || source.height <= 0 || source.layer >= source.layers) {
			OOVR_LOG_ONCEF("Graphics interop: an OpenGL image that can't be read (%dx%d, slice %d of %d): frame left out", source.width,
			    source.height, source.layer, source.layers);
			return false;
		}
		return true;
	}

	Copy* CopyFor(GLuint src, const Source& source)
	{
		const std::pair<GLuint, Kind> key{ src, source.kind };
		auto it = copies.find(key);
		if (it != copies.end() && it->second.width == source.width && it->second.height == source.height && it->second.format == source.format)
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

		DXGI_FORMAT dxgi = DxgiOfGl(source.format);
		if (dxgi == DXGI_FORMAT_UNKNOWN) {
			OOVR_LOG_ONCEF("Graphics interop: an OpenGL image of internal format 0x%x (%dx%d) can't be copied", source.format, source.width, source.height);
			return nullptr;
		}
		Copy copy;
		copy.width = source.width;
		copy.height = source.height;
		copy.format = source.format;
		copy.tex = MakeTexture(device, source.width, source.height, dxgi, 0);
		if (copy.tex) {
			glGenTextures(1, &copy.name);
			copy.object = registerObject(interopDevice, copy.tex.Get(), copy.name, GL_TEXTURE_2D, kAccessWriteDiscard);
		}
		if (!copy.object) {
			OOVR_LOG_ONCEF("Graphics interop: an OpenGL image of internal format 0x%x couldn't be shared with D3D11 (DXGI format %d)", source.format, (int)dxgi);
			Drop(copy);
			return nullptr;
		}
		return &(copies[key] = std::move(copy));
	}

	Copied CopyIn(const vr::Texture_t* tex, vr::EVRSubmitFlags submitFlags, int eye) override
	{
		GLuint src = (GLuint)(intptr_t)tex->handle;
		Source source;
		const bool known = Identify(src, submitFlags, eye, source);
		Copy* copy = known ? CopyFor(src, source) : nullptr;
		if (!copy) {
			// The tries' errors aren't the game's
			for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
			}
			return {};
		}
		if (!lockObjects(interopDevice, 1, &copy->object)) {
			OOVR_LOG_ONCE("Graphics interop: wglDXLockObjectsNV failed: frame left out");
			return {};
		}
		// The game's own earlier errors aren't this copy's (at most 32: the context reports each one once)
		for (int i = 0; i < 32 && glGetError() != GL_NO_ERROR; i++) {
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
		switch (source.kind) {
		case Kind::Renderbuffer:
			framebufferRenderbuffer(kReadFramebuffer, kColorAttachment0, kRenderbuffer, src);
			break;
		case Kind::Array:
			framebufferTextureLayer(kReadFramebuffer, kColorAttachment0, src, 0, source.layer);
			break;
		case Kind::Multisampled:
			framebufferTexture2D(kReadFramebuffer, kColorAttachment0, kTexture2DMultisample, src, 0);
			break;
		default:
			framebufferTexture2D(kReadFramebuffer, kColorAttachment0, GL_TEXTURE_2D, src, 0);
			break;
		}
		bindFramebuffer(kDrawFramebuffer, framebuffers[1]);
		framebufferTexture2D(kDrawFramebuffer, kColorAttachment0, GL_TEXTURE_2D, copy->name, 0);
		// GL's rows run bottom-up, D3D's top-down: turned over as it's copied. A blit that resolves samples can't also
		// turn them over: copied as they are, and the bounds turn them.
		const bool resolves = source.kind == Kind::Renderbuffer || source.kind == Kind::Multisampled;
		if (resolves)
			blitFramebuffer(0, 0, copy->width, copy->height, 0, 0, copy->width, copy->height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		else
			blitFramebuffer(0, 0, copy->width, copy->height, 0, copy->height, copy->width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		if (source.kind == Kind::Renderbuffer)
			framebufferRenderbuffer(kReadFramebuffer, kColorAttachment0, kRenderbuffer, 0);
		else
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
			return {};
		}
		return { copy->tex.Get(), resolves };
	}
};

} // namespace

std::unique_ptr<GfxInterop> GfxInterop::ForGL(ID3D11Device* onDevice)
{
	if (!wglGetCurrentContext()) {
		OOVR_LOG("Graphics interop: no current OpenGL context");
		return nullptr;
	}
	auto interop = std::make_unique<GLInterop>();
	interop->UseDevice(onDevice);
	if (!interop->Init())
		return nullptr;
	OOVR_LOG("Graphics interop: OpenGL frames go through Direct3D 11");
	return interop;
}

#else

std::unique_ptr<GfxInterop> GfxInterop::ForGL(ID3D11Device*)
{
	return nullptr;
}

#endif

// --- Vulkan, importing D3D11 textures through VK_KHR_external_memory_win32 ---

#ifdef SUPPORT_VK

namespace {

// The DXGI format of the same texels (a copy can't convert), UNKNOWN if there's none
DXGI_FORMAT DxgiOfVk(VkFormat format)
{
	std::optional<int64_t> dxgi = SameTexelsIn(GfxApiKind::Vulkan, (int64_t)format, GfxApiKind::D3D11);
	return dxgi ? (DXGI_FORMAT)*dxgi : DXGI_FORMAT_UNKNOWN;
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
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
		return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return DXGI_FORMAT_R32G32B32A32_TYPELESS;
	default:
		return format;
	}
}

// How long a copy may take on the game's queue: far more than one does, so a longer one is a stuck GPU
constexpr uint64_t kCopyTimeoutNs = 100'000'000;
constexpr uint64_t kShutdownTimeoutNs = 5'000'000'000;

class VkInterop : public GfxInterop {
public:
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice vkDevice = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkCommandPool pool = VK_NULL_HANDLE;
	// The copies' command buffers and fences, used in turn: a frame's two eyes, two frames running. A copy waits only
	// for its slot's copy before, not for the other eye's of the same frame, which still waits behind the game's work
	// when the GPU wait is used. pending: submitted, and its fence not seen signalled yet (its command buffer, and the
	// images it copies, are in use until it is).
	struct Slot {
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool pending = false;
	};
	static constexpr int kSlots = 4;
	Slot slots[kSlots];
	int nextSlot = 0;
	PFN_vkGetMemoryWin32HandlePropertiesKHR getHandleProperties = nullptr;
	// The GPU-side wait, where the game's device has VK_KHR_external_semaphore_win32 and D3D11 has shared fences
	// (ID3D11Device5): fences[0] is signalled by D3D11 once it's done with the copies so far, which the Vulkan copy
	// waits for; fences[1] by the Vulkan copy, which D3D11 waits for. Both are imported into the game's device as
	// semaphores. Without them, the copy is waited for on the CPU.
	ComPtr<ID3D11Fence> fences[2];
	VkSemaphore semaphores[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	ComPtr<ID3D11DeviceContext4> context4;
	uint64_t fenceValue = 0;
	bool gpuWait = false;

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

	// Waits up to timeoutNs for the slot's copy: VK_SUCCESS once it's done (or there was none), VK_TIMEOUT while it
	// still runs, when nothing it uses may be reset or freed, or the error of a lost device, which ends it.
	VkResult WaitForSlot(Slot& slot, uint64_t timeoutNs)
	{
		if (!slot.pending)
			return VK_SUCCESS;
		VkResult result = vkWaitForFences(vkDevice, 1, &slot.fence, VK_TRUE, timeoutNs);
		if (result != VK_TIMEOUT)
			slot.pending = false;
		return result;
	}

	// The same for every copy submitted: before the copies' images or the command buffers are freed
	VkResult WaitForCopies(uint64_t timeoutNs)
	{
		for (Slot& slot : slots) {
			VkResult result = WaitForSlot(slot, timeoutNs);
			if (result == VK_TIMEOUT)
				return result;
		}
		return VK_SUCCESS;
	}

	~VkInterop() override
	{
		if (WaitForCopies(kShutdownTimeoutNs) == VK_TIMEOUT) {
			// Still running on the game's queue: its objects are left to the device
			OOVR_LOG("Graphics interop: the last Vulkan copy never finished; its objects aren't freed");
			return;
		}
		for (auto& kv : copies)
			Drop(kv.second);
		for (VkSemaphore semaphore : semaphores) {
			if (semaphore)
				vkDestroySemaphore(vkDevice, semaphore, nullptr);
		}
		for (Slot& slot : slots) {
			if (slot.fence)
				vkDestroyFence(vkDevice, slot.fence, nullptr);
		}
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

		// Signalled, so a teardown before the first copy doesn't wait for them (CopyIn resets one before each submit)
		VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		for (Slot& slot : slots) {
			if (vkCreateFence(vkDevice, &fenceInfo, nullptr, &slot.fence) != VK_SUCCESS)
				return false;
		}
		gpuWait = SetUpGpuWait();
		OOVR_LOGF("Graphics interop: Vulkan copies are waited for %s", gpuWait ? "on the GPU (shared fences)" : "on the CPU");
		return true;
	}

	// The shared fences and their semaphores: false (with none left) where either side lacks them
	bool SetUpGpuWait()
	{
		auto importSemaphore = (PFN_vkImportSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(vkDevice, "vkImportSemaphoreWin32HandleKHR");
		ComPtr<ID3D11Device5> device5;
		ComPtr<ID3D11DeviceContext> context;
		device->GetImmediateContext(&context);
		if (!importSemaphore || FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || !context || FAILED(context.As(&context4)))
			return false;
		bool ok = true;
		for (int i = 0; ok && i < 2; i++) {
			HANDLE handle = nullptr;
			ok = SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fences[i])))
			    && SUCCEEDED(fences[i]->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle));
			VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
			ok = ok && vkCreateSemaphore(vkDevice, &semaphoreInfo, nullptr, &semaphores[i]) == VK_SUCCESS;
			if (ok) {
				VkImportSemaphoreWin32HandleInfoKHR import{ VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
				import.semaphore = semaphores[i];
				import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D11_FENCE_BIT;
				import.handle = handle;
				ok = importSemaphore(vkDevice, &import) == VK_SUCCESS;
			}
			// Importing doesn't take the handle over
			if (handle)
				CloseHandle(handle);
		}
		if (!ok) {
			OOVR_LOG("Graphics interop: no shared fences between D3D11 and the game's Vulkan device");
			for (VkSemaphore& semaphore : semaphores) {
				if (semaphore)
					vkDestroySemaphore(vkDevice, semaphore, nullptr);
				semaphore = VK_NULL_HANDLE;
			}
			fences[0].Reset();
			fences[1].Reset();
			context4.Reset();
		}
		return ok;
	}

	// The slots' command buffers, of the game's queue's family (which comes with each texture)
	bool CommandsFor(uint32_t queueFamily)
	{
		if (pool && family == queueFamily)
			return true;
		if (WaitForCopies(kCopyTimeoutNs) == VK_TIMEOUT)
			return false;
		if (pool)
			vkDestroyCommandPool(vkDevice, pool, nullptr);
		pool = VK_NULL_HANDLE;
		for (Slot& slot : slots)
			slot.commands = VK_NULL_HANDLE;
		family = queueFamily;
		VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = family;
		if (vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
			pool = VK_NULL_HANDLE;
			return false;
		}
		VkCommandBuffer buffers[kSlots] = {};
		VkCommandBufferAllocateInfo bufferInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		bufferInfo.commandPool = pool;
		bufferInfo.commandBufferCount = kSlots;
		bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		if (vkAllocateCommandBuffers(vkDevice, &bufferInfo, buffers) != VK_SUCCESS) {
			vkDestroyCommandPool(vkDevice, pool, nullptr);
			pool = VK_NULL_HANDLE;
			return false;
		}
		for (int i = 0; i < kSlots; i++)
			slots[i].commands = buffers[i];
		return true;
	}

	Copy* CopyFor(const vr::VRVulkanTextureData_t* data)
	{
		VkImage src = (VkImage)data->m_nImage;
		auto it = copies.find(src);
		if (it != copies.end() && it->second.width == data->m_nWidth && it->second.height == data->m_nHeight
		    && it->second.format == (VkFormat)data->m_nFormat)
			return &it->second;
		// A copy freed below may still be written by one running
		if ((it != copies.end() || copies.size() >= kMaxCopies) && WaitForCopies(kCopyTimeoutNs) == VK_TIMEOUT) {
			OOVR_LOG_ONCE("Graphics interop: an earlier Vulkan copy is still running: frame left out");
			return nullptr;
		}
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

	Copied CopyIn(const vr::Texture_t* tex, vr::EVRSubmitFlags submitFlags, int) override
	{
		auto* data = (const vr::VRVulkanTextureData_t*)tex->handle;
		// The layer: SubmitWithArrayIndex's, else the array data's, else the first
		uint32_t layer = 0;
		if (std::optional<uint32_t> slice = SubmitArraySlice::Current())
			layer = *slice;
		else if (submitFlags & vr::Submit_VulkanTextureWithArrayData)
			layer = ((const vr::VRVulkanTextureArrayData_t*)data)->m_unArrayIndex;
		// The slot's command buffer is reused: its copy before (two frames ago) must be over
		Slot& slot = slots[nextSlot];
		if (WaitForSlot(slot, kCopyTimeoutNs) == VK_TIMEOUT) {
			OOVR_LOG_ONCE("Graphics interop: an earlier Vulkan copy is still running: frame left out");
			return {};
		}
		if (!CommandsFor(data->m_nQueueFamilyIndex)) {
			OOVR_LOG_ONCE("Graphics interop: no Vulkan command buffer for the game's queue family");
			return {};
		}
		Copy* copy = CopyFor(data);
		if (!copy)
			return {};

		vkResetCommandBuffer(slot.commands, 0);
		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(slot.commands, &begin);

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
		vkCmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

		VkImageSubresourceLayers from{ VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
		VkImageSubresourceLayers layers{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		if (data->m_nSampleCount > 1) {
			VkImageResolve region{ from, { 0, 0, 0 }, layers, { 0, 0, 0 }, { copy->width, copy->height, 1 } };
			vkCmdResolveImage(slot.commands, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		} else {
			VkImageCopy region{ from, { 0, 0, 0 }, layers, { 0, 0, 0 }, { copy->width, copy->height, 1 } };
			vkCmdCopyImage(slot.commands, (VkImage)data->m_nImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy->image,
			    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
		}

		VkImageMemoryBarrier release = toDst;
		release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		release.dstAccessMask = 0;
		release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		release.srcQueueFamilyIndex = family;
		release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
		vkCmdPipelineBarrier(slot.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
		vkEndCommandBuffer(slot.commands);

		VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &slot.commands;
		// On the GPU: the copy waits for D3D11 to be done with the copies so far (it may still read last frame's), and
		// D3D11 waits for the copy. Signal goes to the GPU with the Flush, which the Vulkan wait needs.
		const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		uint64_t readyValue = 0, copiedValue = 0;
		VkD3D12FenceSubmitInfoKHR values{ VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR };
		bool waitOnGpu = gpuWait;
		if (waitOnGpu) {
			readyValue = ++fenceValue;
			copiedValue = ++fenceValue;
			waitOnGpu = SUCCEEDED(context4->Signal(fences[0].Get(), readyValue));
			context4->Flush();
		}
		if (waitOnGpu) {
			values.waitSemaphoreValuesCount = 1;
			values.pWaitSemaphoreValues = &readyValue;
			values.signalSemaphoreValuesCount = 1;
			values.pSignalSemaphoreValues = &copiedValue;
			submit.pNext = &values;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &semaphores[0];
			submit.pWaitDstStageMask = &waitStage;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &semaphores[1];
		}
		vkResetFences(vkDevice, 1, &slot.fence);
		// On the queue that comes with the texture, the game's own
		VkQueue target = data->m_pQueue ? (VkQueue)data->m_pQueue : queue;
		if (vkQueueSubmit(target, 1, &submit, slot.fence) != VK_SUCCESS) {
			OOVR_LOG_ONCE("Graphics interop: vkQueueSubmit failed: frame left out");
			return {};
		}
		slot.pending = true;
		nextSlot = (nextSlot + 1) % kSlots;
		// Done before D3D11 reads it: D3D11's own work waits on the GPU, or this thread waits for it
		if (waitOnGpu && SUCCEEDED(context4->Wait(fences[1].Get(), copiedValue)))
			return { copy->tex.Get(), false };
		if (WaitForSlot(slot, kCopyTimeoutNs) != VK_SUCCESS) {
			OOVR_LOG_ONCE("Graphics interop: the Vulkan copy didn't finish in time: frame left out");
			return {};
		}
		return { copy->tex.Get(), false };
	}
};

} // namespace

std::unique_ptr<GfxInterop> GfxInterop::ForVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, ID3D11Device* onDevice)
{
	if (!instance || !physicalDevice || !device || !queue)
		return nullptr;
	auto interop = std::make_unique<VkInterop>();
	interop->UseDevice(onDevice);
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

std::unique_ptr<GfxInterop> GfxInterop::ForGL(ID3D11Device*)
{
	return nullptr;
}

#ifdef SUPPORT_VK
std::unique_ptr<GfxInterop> GfxInterop::ForVulkan(VkInstance, VkPhysicalDevice, VkDevice, VkQueue, ID3D11Device*)
{
	return nullptr;
}
#endif

bool GfxInterop::MakeDevice()
{
	return false;
}

void GfxInterop::UseDevice(ID3D11Device*)
{
}

#endif
