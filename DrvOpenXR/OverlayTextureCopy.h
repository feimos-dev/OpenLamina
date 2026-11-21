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

	// On the program's thread: tex's first layer, whole, in a texture of the device (bounds isn't read: the caller
	// selects the bounds' area of the copy as the layer's image rectangle), valid
	// until the next copy of the same texture; nullptr, with why, if it can't be copied. owner is whose texture it is
	// (an overlay), for Forget. The textures it shares for copying are kept for the next copy of the same texture, at
	// most 16 of each kind: the one used longest ago then makes room.
	ID3D11Texture2D* CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, std::string* why, const void* owner = nullptr);
	// On the program's thread: what was kept for the owner's Direct3D textures is let go (it shows none any more)
	void Forget(const void* owner);

	// On the program's thread: a texture of the program's device that the device reads too, for a program that reads
	// its overlay's texture back (GetOverlayTexture) and then writes it without another call. program is a new texture
	// of like's device, size and format with like's first layer copied in, shared by a DXGI handle; session is the same
	// texture opened on the device. Both are referenced for the caller. False, with why, if it can't be made.
	bool MakeLive(ID3D11Texture2D* like, ID3D11Texture2D** program, ID3D11Texture2D** session, std::string* why);
	// A texture shared by a DXGI handle (TextureType_DXGISharedHandle) opened on the device, referenced for the caller;
	// nullptr, with why, if it can't be opened
	ID3D11Texture2D* OpenShared(void* handle, std::string* why);

	// The device it copies onto
	ID3D11Device* Device() const { return device; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	ID3D11Device* device = nullptr;
};
