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
	// WGL_NV_DX_interop2, with the game's OpenGL context current; nullptr (logged) if the GL driver lacks it
	static std::unique_ptr<GfxInterop> ForGL();
#ifdef SUPPORT_VK
	// VK_KHR_external_memory_win32 importing a D3D11 NT handle; nullptr (logged) if the game's device lacks it, or
	// isn't on the runtime's adapter
	static std::unique_ptr<GfxInterop> ForVulkan(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, VkQueue queue);
#endif

	// The D3D11 texture holding this frame's copy of tex (the whole of its first layer, upright as shown, which the
	// bounds then apply to); nullptr (logged) if it couldn't be copied
	virtual ID3D11Texture2D* CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds) = 0;

	// The texture type copied through D3D11
	virtual vr::ETextureType Type() const = 0;

	// The D3D11 device on the runtime's adapter, which the session is on
	ID3D11Device* Device() const { return device; }

	virtual ~GfxInterop();

protected:
	// Makes device, on the runtime's adapter (D3D11 requirements); false (logged) if it can't
	bool MakeDevice();

	ID3D11Device* device = nullptr;
};
