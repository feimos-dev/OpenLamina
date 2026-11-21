#pragma once

#include "dxcompositor.h"

#include "dx11compositor.h"

#include <d3d10_1.h>

#include <map>

/**
 * A game's Direct3D 10 textures, shown through the Direct3D 11 path: the session is on a D3D11 device of the D3D10
 * device's adapter (BridgeDevice). Each texture the game submits gets a shared texture with a keyed mutex, made on
 * the game's D3D10 device and opened on the D3D11 one. A Submit copies the game's texture into it (key 0 to 1 on the
 * D3D10 side), and the D3D11 side then copies that into the swapchain as for a D3D11 texture (key 1 to 0).
 */
class DX10Compositor : public DX11Compositor {
public:
	// initial is one of the game's textures, whose device's BridgeDevice the session is on: nullptr (logged) where that
	// device has no bridge, or a texture can't be made on it
	static std::unique_ptr<DX10Compositor> Create(ID3D10Texture2D* initial);

	void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	// Through the shared copy of its first slice
	bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out) override;

	// D3D10 faces aren't drawn on the GPU: they're read back and joined on the CPU
	bool InvokeCubemap(const vr::Texture_t* faces, uint32_t maxWidth, XrSwapchainSubImage& subImage) override { return false; }

	// The texture of a TextureType_DirectX Texture_t that is a D3D10 one (not a D3D11 one), else nullptr
	static ID3D10Texture2D* AsD3D10Texture(const vr::Texture_t* texture);

	// The D3D11 device on the adapter of tex's D3D10 device, made the first time for that device (logged on failure:
	// nullptr). Kept until ReleaseBridgeDevice; until then, nullptr (logged once) for another D3D10 device's texture.
	static ID3D11Device* BridgeDevice(ID3D10Texture2D* tex);
	// After the session on it has been destroyed
	static void ReleaseBridgeDevice();

private:
	// texel: a texture of the bridge device
	explicit DX10Compositor(ID3D11Texture2D* texel);

	// One of the game's textures and its shared copy
	struct Shared {
		D3D10_TEXTURE2D_DESC gameDesc{};
		ComPtr<ID3D10Texture2D> tex10;
		ComPtr<IDXGIKeyedMutex> mutex10;
		ComPtr<ID3D11Texture2D> tex11;
		ComPtr<IDXGIKeyedMutex> mutex11;
	};
	// The shared copy for game (made, or made again when its description changed), nullptr if it can't be made
	Shared* SharedFor(ID3D10Texture2D* game);
	// Copies subresource of game (its mip 0 of one slice) into s and takes the D3D11 side's key, which EndShared hands
	// back: false (logged once; s may be gone) if the copy didn't get across
	bool CopyIntoShared(ID3D10Texture2D* game, Shared& s, UINT subresource);
	void EndShared(Shared& s);

	// By the game's texture, which the game owns (a texture freed and another made at its address gets a new copy if
	// its description differs)
	std::map<ID3D10Texture2D*, Shared> shared;
};
