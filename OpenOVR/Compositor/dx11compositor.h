#pragma once

#include "dxcompositor.h"

class DX11Compositor : public Compositor {
public:
	DX11Compositor(ID3D11Texture2D* td);

	virtual ~DX11Compositor() override;

	// Override
	virtual void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;


	bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out) override;

protected:
	bool CopyDepth(const vr::Texture_t* depth) override;

public:

	ID3D11Device* GetDevice() { return device; }

	// IVRCompositor::GetMirrorTextureD3D11's texture of an eye: a texture of OpenLamina's own on the game's device, which
	// each frame the game submits is copied into, so a view of it shows the game's latest frame for as long as it's held.
	//
	// MirrorUpdate copies the bounds' area of a submitted texture (the game's ID3D11Texture2D; a multisampled one is
	// resolved whole) into *mirror, as much of it as the mirror's size holds. *mirror is only replaced where it isn't
	// a texture of that device (views handed out before then keep the old one); a frame of a format family other than
	// its own is left out.
	static void MirrorUpdate(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, ID3D11Texture2D** mirror);
	// A view of *mirror for deviceOrResource's device (an ID3D11Device, or a resource of it): an OpenVR compositor
	// error, with *view set only on success (the caller releases it). *mirror is made here if there is none, width x
	// height of `format` (the format of the game's frames, from MirrorFormat; DXGI_FORMAT_UNKNOWN: 8-bit sRGB RGBA).
	static int MirrorView(ID3D11Texture2D** mirror, IUnknown* deviceOrResource, uint32_t width, uint32_t height, DXGI_FORMAT format, ID3D11ShaderResourceView** view);
	// The format a mirror texture of the frames of this texture has (DXGI_FORMAT_UNKNOWN if it isn't a D3D11 one)
	static DXGI_FORMAT MirrorFormat(const vr::Texture_t* texture);

protected:
	// A swapchain for the texture, width x height (0: the texture's size)
	void CheckCreateSwapChain(const vr::Texture_t* texture, uint32_t width, uint32_t height, bool cube);


	bool CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace);

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	ID3D11ShaderResourceView* quad_texture_view;
	ID3D11SamplerState* quad_sampleState = nullptr;
	ID3D11VertexShader* fs_vshader = nullptr;
	ID3D11PixelShader* fs_pshader = nullptr;
	// False if the shaders above couldn't be made: textures are then copied without them
	bool shadersUsable = false;
	ID3D11Buffer* constBuffer = nullptr; // the source area for fs_vshader

	std::vector<XrSwapchainImageD3D11KHR> imagesHandles;
	std::vector<ID3D11RenderTargetView*> swapchain_rtvs;
	std::vector<ID3D11Texture2D*> resolvedMSAATextures;

	// The swapchain's format isn't the texture's family: it goes through the shader pass
	bool convertFormat = false;
	// The colour space the swapchain was made for
	vr::EColorSpace createColourSpace = vr::ColorSpace_Auto;
	// What the shader pass reads when the texture itself can't be (multisampled, or not a shader resource)
	ID3D11Texture2D* shaderSource = nullptr;
	ID3D11Texture2D* ShaderSource(const D3D11_TEXTURE2D_DESC& of, DXGI_FORMAT format);
	// The shader pass's own pipeline state (D3D11.1), swapped in for it
	ID3DDeviceContextState* ownState = nullptr;
	ID3D11DeviceContext1* context1 = nullptr;

	struct ShaderPass {
		// The source area: uv = offset + scale * (0..1 across the image)
		float offsetX = 0, offsetY = 0, scaleX = 1, scaleY = 1;
		bool decodeSrgb = false, encodeSrgb = false;
		// The area drawn, from the image's origin
		uint32_t width = 0, height = 0;
	};
	// Draws view over pass's area of swapchain image index (its render target view)
	void DrawWithShader(ID3D11ShaderResourceView* view, uint32_t index, const ShaderPass& pass);
};
