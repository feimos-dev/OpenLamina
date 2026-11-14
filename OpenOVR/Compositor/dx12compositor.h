#pragma once

#include "dxcompositor.h"

class DX12Compositor : public Compositor {
public:
	DX12Compositor(vr::D3D12TextureData_t* td);

	// Override
	virtual ~DX12Compositor() override;

	// Override
	void CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags) override;

	void InvokeCubemap(const vr::Texture_t* textures) override;
	virtual bool SupportsCubemap() override { return false; }

	bool ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out) override;

	ComPtr<ID3D12Device> GetDevice() { return device; }

private:
	void CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube);


	bool CheckChainCompatible(D3D12_RESOURCE_DESC& inputDesc, vr::EColorSpace colourSpace);

	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;

	std::vector<ComPtr<ID3D12CommandAllocator>> commandAllocators;
	std::vector<ComPtr<ID3D12GraphicsCommandList>> commandLists;

	std::vector<HANDLE> frameFenceEvents;
	std::vector<ComPtr<ID3D12Fence>> frameFences;
	std::vector<UINT64> fenceValues;
	UINT64 currentFenceValue;

	std::vector<XrSwapchainImageD3D12KHR> imagesHandles;

	// For work that's waited for at once (ReadbackToCpu): its own allocator, list and fence, made at first use
	ComPtr<ID3D12CommandAllocator> oneShotAllocator;
	ComPtr<ID3D12GraphicsCommandList> oneShotList;
	ComPtr<ID3D12Fence> oneShotFence;
	HANDLE oneShotEvent = nullptr;
	UINT64 oneShotValue = 0;
	// A reset list for one-shot work, or nullptr
	ID3D12GraphicsCommandList* BeginOneShot();
	// Closes the list, runs it on queue and waits for it (at most 5 s): false if it couldn't be run or timed out
	bool EndOneShot(ID3D12CommandQueue* queue);

	struct DxgiFormatInfo {
		/// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
		/// Both the SRGB and linear formats should be UNORM.
		DXGI_FORMAT srgb, linear, typeless;

		/// The bits per pixel, bits per channel, and the number of channels
		int bpp, bpc, channels;
	};

	/**
	 * Gets information about a given format into the output variable. Returns true if the texture was
	 * found, if not it returns false and leaves out in an undefined state.
	 */
	static bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo& out);
};
