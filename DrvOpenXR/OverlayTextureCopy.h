#pragma once
// An overlay program's textures, copied on its own thread into textures of the overlay session's device: OpenLamina's
// own D3D11 device on the runtime's adapter. The session's frames, which OpenLamina runs on a thread of its own, then
// never use a device, context or queue of the program's.
//   - D3D11: through a texture of the program's device shared with a keyed mutex
//   - D3D12: through a resource shared from the program's device, copied on its queue and waited for
//   - OpenGL and Vulkan: through GfxInterop (as a game's frames are copied through D3D11)
// Anything else (Direct3D 10, a device on another adapter, a format that can't be shared) isn't copied.

#include "generated/interfaces/vrtypes.h"

#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11Texture2D;

class OverlayTextureCopy {
public:
	// Copies onto device (referenced)
	explicit OverlayTextureCopy(ID3D11Device* device);
	~OverlayTextureCopy();
	OverlayTextureCopy(const OverlayTextureCopy&) = delete;
	OverlayTextureCopy& operator=(const OverlayTextureCopy&) = delete;

	// On the program's thread: tex's first layer (bounds as for GfxInterop::CopyIn) in a texture of the device, valid
	// until the next copy of the same texture; nullptr, with why, if it can't be copied
	ID3D11Texture2D* CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, std::string* why);

	// The device it copies onto
	ID3D11Device* Device() const { return device; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	ID3D11Device* device = nullptr;
};
