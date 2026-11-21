#pragma once
// When the runtime lacks XR_KHR_opengl_enable or XR_KHR_vulkan_enable but has XR_KHR_D3D11_enable: the session is D3D11
// on the runtime's adapter, and each GL or Vulkan texture is copied through a shared D3D11 texture

#include "generated/interfaces/vrtypes.h"

#include <memory>

#ifdef SUPPORT_VK
#include <vulkan/vulkan.h>
#endif

struct ID3D11Device;
struct ID3D11Texture2D;

class GfxInterop {
public:
	// WGL_NV_DX_interop2, with the game's OpenGL context current; nullptr (logged) if the GL driver lacks it. On
	// onDevice where given (a D3D11 device on the runtime's adapter), else on a device of its own.
	static std::unique_ptr<GfxInterop> ForGL(ID3D11Device* onDevice = nullptr);
#ifdef SUPPORT_VK
	// VK_KHR_external_memory_win32 importing a D3D11 NT handle; nullptr (logged) if the game's device lacks it, or
	// isn't on the runtime's adapter
	static std::unique_ptr<GfxInterop> ForVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue, ID3D11Device* onDevice = nullptr);
#endif

	// What CopyIn made: the D3D11 texture holding this frame's copy (nullptr, logged, if it couldn't be copied), and
	// whether it lies with the source's rows as they are. That's upside down as shown for an OpenGL source read from
	// samples (a renderbuffer, a multisampled texture), which a blit resolves but can't also turn over: the bounds turn
	// it then.
	struct Copied {
		ID3D11Texture2D* texture = nullptr;
		bool upsideDown = false;
	};
	// Copies the whole of tex's layer that's shown: SubmitWithArrayIndex's slice, else the one the submit flags name
	// (Submit_GlArrayTexture: the eye's; Submit_VulkanTextureWithArrayData: the data's), else the first. Upright as
	// shown (but see Copied), which the bounds then apply to.
	virtual Copied CopyIn(const vr::Texture_t* tex, vr::EVRSubmitFlags submitFlags, int eye) = 0;

	// The texture type copied through D3D11
	virtual vr::ETextureType Type() const = 0;

	// The D3D11 device on the runtime's adapter, which the session is on
	ID3D11Device* Device() const { return device; }

	virtual ~GfxInterop();

protected:
	// Makes device, on the runtime's adapter (D3D11 requirements), unless it was given; false (logged) if it can't
	bool MakeDevice();
	// Uses onDevice (referenced) in place of a device of its own
	void UseDevice(ID3D11Device* onDevice);

	ID3D11Device* device = nullptr;
};
