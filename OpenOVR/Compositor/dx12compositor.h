#pragma once

#include "dxcompositor.h"
#include "dx12convert.h"

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

protected:
	bool CopyDepth(const vr::Texture_t* depth) override;

public:

	ComPtr<ID3D12Device> GetDevice() { return device; }

private:
	// A swapchain for the texture, width x height (0: the texture's size)
	void CheckCreateSwapChain(const vr::Texture_t* texture, uint32_t width, uint32_t height, bool cube);


	bool CheckChainCompatible(D3D12_RESOURCE_DESC& inputDesc, vr::EColorSpace colourSpace);

	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandQueue> queue;

	std::vector<ComPtr<ID3D12CommandAllocator>> commandAllocators;
	std::vector<ComPtr<ID3D12GraphicsCommandList>> commandLists;

	std::vector<HANDLE> frameFenceEvents;
	std::vector<ComPtr<ID3D12Fence>> frameFences;
	std::vector<UINT64> fenceValues;
	UINT64 currentFenceValue = 0;

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

	// The swapchain's format isn't the texture's family: it goes through the shader pass
	bool convertFormat = false;
	// The colour space the swapchain was made for
	vr::EColorSpace createColourSpace = vr::ColorSpace_Auto;
	std::unique_ptr<Dx12Convert> pass;
	// A multisampled texture resolved (kept in RESOLVE_DEST between frames)
	ComPtr<ID3D12Resource> resolved;
	ID3D12Resource* Resolved(const D3D12_RESOURCE_DESC& of, DXGI_FORMAT format);
	// Waits for every image's last copy
	void WaitForAllFrames();
	// A command allocator, list and fence for each of count swapchain images
	void MakeFrameLists(uint32_t count);
};
