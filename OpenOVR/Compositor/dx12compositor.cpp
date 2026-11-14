#include "stdafx.h"

#include <algorithm>

#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)

#include "dx12compositor.h"

#include <string>

#include <atlbase.h>

#include <DirectXMath.h>
#include <d3dcompiler.h>

#include <d3d12.h>

#include "FormatTable.h"

#include "../Misc/Config.h"
#include "../Misc/SoftFail.h"
#include "../Misc/xr_ext.h"

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

using namespace vr;
using namespace std;
using glm::min;

namespace {
void WaitForFence(ID3D12Fence* fence, UINT64 completionValue, HANDLE waitEvent)
{
	if (fence->GetCompletedValue() < completionValue) {
		fence->SetEventOnCompletion(completionValue, waitEvent);
		WaitForSingleObject(waitEvent, INFINITE);
	}
}
} // namespace

DX12Compositor::DX12Compositor(D3D12TextureData_t* td)
{
	queue = td->m_pCommandQueue;
	queue->GetDevice(IID_PPV_ARGS(&device));
}

DX12Compositor::~DX12Compositor()
{
	for (auto event : frameFenceEvents) {
		CloseHandle(event);
	}
	if (oneShotEvent)
		CloseHandle(oneShotEvent);
}

ID3D12GraphicsCommandList* DX12Compositor::BeginOneShot()
{
	if (!oneShotList) {
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&oneShotAllocator)))
		    || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, oneShotAllocator.Get(), nullptr, IID_PPV_ARGS(&oneShotList)))
		    || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&oneShotFence)))) {
			oneShotList.Reset();
			return nullptr;
		}
		oneShotList->Close();
		oneShotEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}
	if (!oneShotEvent || FAILED(oneShotAllocator->Reset()) || FAILED(oneShotList->Reset(oneShotAllocator.Get(), nullptr)))
		return nullptr;
	return oneShotList.Get();
}

bool DX12Compositor::EndOneShot(ID3D12CommandQueue* q)
{
	if (FAILED(oneShotList->Close()))
		return false;
	ID3D12CommandList* lists[] = { oneShotList.Get() };
	q->ExecuteCommandLists(1, lists);
	const UINT64 value = ++oneShotValue;
	if (FAILED(q->Signal(oneShotFence.Get(), value)))
		return false;
	if (oneShotFence->GetCompletedValue() < value) {
		oneShotFence->SetEventOnCompletion(value, oneShotEvent);
		if (WaitForSingleObject(oneShotEvent, 5000) != WAIT_OBJECT_0)
			return false;
	}
	return true;
}

D3D12_RESOURCE_BARRIER Dx12Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, UINT subresource)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = resource;
	b.Transition.Subresource = subresource;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	return b;
}

bool DX12Compositor::ReadbackToCpu(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, Image& out)
{
	// The game's texture is in PIXEL_SHADER_RESOURCE (as OpenVR's D3D12 textures are given), and is put back there
	auto* input = tex ? (D3D12TextureData_t*)tex->handle : nullptr;
	if (!input || !input->m_pResource || !input->m_pCommandQueue)
		return false;
	D3D12_RESOURCE_DESC desc = input->m_pResource->GetDesc();
	const TexelLayout layout = TexelLayoutOfDxgi(desc.Format);
	if (layout == TexelLayout::Unknown || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		OOVR_LOG_ONCEF("A D3D12 texture of format %d can't be read back into memory", (int)desc.Format);
		return false;
	}
	TextureRegion region;
	if (!RegionFromBounds(bounds, (uint32_t)desc.Width, desc.Height, region))
		return false;

	D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT };
	D3D12_HEAP_PROPERTIES readbackHeap{ D3D12_HEAP_TYPE_READBACK };
	const DXGI_FORMAT typed = TypedDxgi(desc.Format);

	// A multisampled texture is resolved first
	ComPtr<ID3D12Resource> resolved;
	if (desc.SampleDesc.Count > 1) {
		D3D12_RESOURCE_DESC rd = desc;
		rd.Format = typed;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc = { 1, 0 };
		rd.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved))))
			return false;
	}

	D3D12_RESOURCE_DESC footprintOf = desc;
	footprintOf.Width = region.Width();
	footprintOf.Height = region.Height();
	footprintOf.DepthOrArraySize = 1;
	footprintOf.MipLevels = 1;
	footprintOf.SampleDesc = { 1, 0 };
	footprintOf.Format = resolved ? typed : desc.Format;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
	UINT64 total = 0;
	device->GetCopyableFootprints(&footprintOf, 0, 1, 0, &footprint, nullptr, nullptr, &total);
	D3D12_RESOURCE_DESC bd{};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = total;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc = { 1, 0 };
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ComPtr<ID3D12Resource> buffer;
	if (FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))))
		return false;

	ID3D12GraphicsCommandList* list = BeginOneShot();
	if (!list) {
		OOVR_LOG_ONCE("Couldn't make a D3D12 command list to read a texture back");
		return false;
	}
	const D3D12_RESOURCE_STATES given = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	ID3D12Resource* from = input->m_pResource;
	if (resolved) {
		D3D12_RESOURCE_BARRIER toResolve = Dx12Transition(from, given, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		list->ResourceBarrier(1, &toResolve);
		list->ResolveSubresource(resolved.Get(), 0, from, 0, typed);
		D3D12_RESOURCE_BARRIER back[2] = { Dx12Transition(from, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, given),
			Dx12Transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE) };
		list->ResourceBarrier(2, back);
		from = resolved.Get();
	} else {
		D3D12_RESOURCE_BARRIER toCopy = Dx12Transition(from, given, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &toCopy);
	}
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = buffer.Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION srcLoc{};
	srcLoc.pResource = from;
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = 0;
	D3D12_BOX box{ region.x0, region.y0, 0, region.x1, region.y1, 1 };
	list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, &box);
	if (!resolved) {
		D3D12_RESOURCE_BARRIER back = Dx12Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, given);
		list->ResourceBarrier(1, &back);
	}
	if (!EndOneShot(input->m_pCommandQueue)) {
		OOVR_LOG_ONCE("A D3D12 texture readback didn't finish");
		return false;
	}
	void* mapped = nullptr;
	D3D12_RANGE range{ 0, (SIZE_T)total };
	if (FAILED(buffer->Map(0, &range, &mapped)))
		return false;
	out = DecodeTexels(layout, (const uint8_t*)mapped + (size_t)footprint.Offset, region.Width(), region.Height(), footprint.Footprint.RowPitch, false,
	    TexelsAreLinear(layout, tex->eColorSpace == vr::ColorSpace_Gamma));
	D3D12_RANGE none{ 0, 0 };
	buffer->Unmap(0, &none);
	FlipImage(out, region.flipX, region.flipY);
	return !out.Empty();
}

void DX12Compositor::WaitForAllFrames()
{
	for (size_t i = 0; i < frameFences.size() && i < fenceValues.size() && i < frameFenceEvents.size(); i++)
		if (frameFences[i])
			WaitForFence(frameFences[i].Get(), fenceValues[i], frameFenceEvents[i]);
}

void DX12Compositor::CheckCreateSwapChain(const vr::Texture_t* texture, uint32_t width, uint32_t height, bool cube)
{
	XrSwapchainCreateInfo& desc = createInfo;

	auto* src = (D3D12TextureData_t*)texture->handle;

	D3D12_RESOURCE_DESC srcDesc;
	srcDesc = src->m_pResource->GetDesc();

	// The swapchain is the size of the area shown
	if (width && height) {
		srcDesc.Width = width;
		srcDesc.Height = height;
	}

	if (cube) {
		// LibOVR can only use square cubemaps, while SteamVR can use any shape
		// Note we use CopySubresourceRegion later on, so this won't cause problems with that
		auto min = std::min(static_cast<UINT64>(srcDesc.Height), srcDesc.Width);
		srcDesc.Width = min;
		srcDesc.Height = static_cast<UINT>(min);
	}

	bool usable = chain == NULL ? false : CheckChainCompatible(srcDesc, texture->eColorSpace);

	if (!usable) {
		OOVR_LOG("Generating new swap chain");

		OOVR_LOGF("Texture desc format: %d", srcDesc.Format);
		OOVR_LOGF("Texture desc flags:  %d", srcDesc.Flags);
		OOVR_LOGF("Texture desc width:  %d", srcDesc.Width);
		OOVR_LOGF("Texture desc height: %d", srcDesc.Height);

		// Nothing of ours may still be running on the old swapchain's images or descriptors
		WaitForAllFrames();

		// First, delete the old chain if necessary
		if (chain) {
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
		}

		// The runtime's format for it: its own, or one it's converted into (by the shader pass)
		std::optional<FormatChoice> choice = ChooseSwapchainFormat(GfxApiKind::D3D12, srcDesc.Format, texture->eColorSpace, RuntimeFormats());
		if (!choice) {
			// Submit answers TextureUsesUnsupportedFormat; the eye is left out
			createInfo = {};
			lastResult = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
			OC_UNSUPPORTED_VOID("A D3D12 texture of DXGI format %d: no swapchain format can show it", (int)srcDesc.Format);
		}
		const DXGI_FORMAT type = (DXGI_FORMAT)choice->swapchainFormat;
		convertFormat = choice->convert;
		if (convertFormat)
			OOVR_LOGF("DXGI format %d goes into swapchain format %d through the shader pass", (int)srcDesc.Format, (int)type);

		// Set aside the old format for checking later
		createInfoFormat = srcDesc.Format;
		createColourSpace = texture->eColorSpace;

		// Make eye render buffer
		desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		// TODO desc.Type = cube ? ovrTexture_Cube : ovrTexture_2D;
		desc.faceCount = cube ? 6 : 1;
		desc.width = static_cast<uint32_t>(srcDesc.Width);
		desc.height = srcDesc.Height;
		desc.format = type;
		// One mip: the copy and the pass write mip 0
		desc.mipCount = 1;
		desc.sampleCount = 1;
		desc.arraySize = 1;
		desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

		ForgetAcquiredImage(); // a new swapchain: no image of the old one is pending
		if (SwapchainCallFailed(xrCreateSwapchain(xr_session.get(), &desc, &chain), "xrCreateSwapchain")) {
			chain = XR_NULL_HANDLE; // tried again at the next Submit
			return;
		}

		// Go through the images and retrieve them - this will be used later in Invoke, since OpenXR doesn't
		// have a convenient way to request one specific image.
		uint32_t imageCount = 0;
		bool listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr), "xrEnumerateSwapchainImages");
		if (listed) {
			imagesHandles = std::vector<XrSwapchainImageD3D12KHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
			listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, imagesHandles.size(), &imageCount, (XrSwapchainImageBaseHeader*)imagesHandles.data()), "xrEnumerateSwapchainImages");
		}
		if (!listed || imageCount != imagesHandles.size()) {
			lastResult = XR_FAILED(lastResult) ? lastResult : XR_ERROR_RUNTIME_FAILURE;
			imagesHandles.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE; // tried again at the next Submit
			return;
		}

		MakeFrameLists(imageCount);

		// The shader pass's descriptors: one slot per image (it's made here for a format it converts, and at the first
		// flipped bounds otherwise)
		if (convertFormat && !pass)
			pass = std::make_unique<Dx12Convert>(device.Get());
		if (pass && pass->Usable() && !pass->EnsureSlots(imageCount))
			pass.reset();
		if (convertFormat) {
			if (!pass || !pass->Usable()) {
				// Submit answers InvalidTexture; the eye is left out
				lastResult = XR_ERROR_VALIDATION_FAILURE;
				OC_UNSUPPORTED_VOID("A D3D12 texture of DXGI format %d needs the shader pass, which couldn't be set up", (int)srcDesc.Format);
			}
		}
		// TODO do we need to release the images at some point, or does the swapchain do that for us?
	}
}

void DX12Compositor::MakeFrameLists(uint32_t imageCount)
{
	for (auto event : frameFenceEvents) {
		CloseHandle(event);
	}

	commandLists.resize(imageCount);
	commandAllocators.resize(imageCount);
	frameFenceEvents.resize(imageCount);
	frameFences.resize(imageCount);
	fenceValues.resize(imageCount);

	for (uint32_t i = 0; i < imageCount; i++) {
		frameFenceEvents[i] = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		fenceValues[i] = 0;
		device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		    IID_PPV_ARGS(&frameFences[i]));

		device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
		    IID_PPV_ARGS(&commandAllocators[i]));
		device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		    commandAllocators[i].Get(), nullptr,
		    IID_PPV_ARGS(&commandLists[i]));
		commandLists[i]->Close();
	}
}

bool DX12Compositor::CopyDepth(const vr::Texture_t* depth)
{
	// The game's depth texture is in PIXEL_SHADER_RESOURCE (as its colour is) and is put back there; the runtime's
	// depth images are in DEPTH_WRITE whenever it has them
	auto* input = (D3D12TextureData_t*)depth->handle;
	if (!input->m_pResource)
		return false;
	D3D12_RESOURCE_DESC desc = input->m_pResource->GetDesc();
	if (desc.SampleDesc.Count > 1) {
		OOVR_LOG_ONCE("A multisampled D3D12 depth texture: not sent to the runtime");
		return false;
	}
	if (!chain || createInfo.width != desc.Width || createInfo.height != desc.Height || createInfoFormat != (int64_t)desc.Format) {
		std::optional<int64_t> format = ChooseDepthSwapchainFormat(GfxApiKind::D3D12, desc.Format, RuntimeFormats());
		if (!format) {
			OOVR_LOG_ONCEF("No depth swapchain format of the runtime's for a D3D12 depth texture of DXGI format %d: depth not sent", (int)desc.Format);
			return false;
		}
		WaitForAllFrames();
		if (!MakeDepthChain(*format, (uint32_t)desc.Width, desc.Height))
			return false;
		createInfoFormat = desc.Format;
		uint32_t count = 0;
		bool listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, 0, &count, nullptr), "xrEnumerateSwapchainImages (depth)");
		if (listed) {
			imagesHandles.assign(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR });
			listed = !SwapchainCallFailed(xrEnumerateSwapchainImages(chain, count, &count, (XrSwapchainImageBaseHeader*)imagesHandles.data()), "xrEnumerateSwapchainImages (depth)");
		}
		if (!listed) {
			imagesHandles.clear();
			xrDestroySwapchain(chain);
			chain = XR_NULL_HANDLE;
			createInfo = {};
			return false;
		}
		MakeFrameLists(count);
	}
	uint32_t index = 0;
	if (!AcquireAndWait((uint32_t)std::min(imagesHandles.size(), frameFences.size()), index))
		return false;
	WaitForFence(frameFences[index].Get(), fenceValues[index], frameFenceEvents[index]);
	commandAllocators[index]->Reset();
	ID3D12GraphicsCommandList* list = commandLists[index].Get();
	list->Reset(commandAllocators[index].Get(), nullptr);
	ID3D12Resource* image = imagesHandles[index].texture;
	const D3D12_RESOURCE_STATES given = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	D3D12_RESOURCE_BARRIER in[2] = { Dx12Transition(input->m_pResource, given, D3D12_RESOURCE_STATE_COPY_SOURCE),
		Dx12Transition(image, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_DEST) };
	list->ResourceBarrier(2, in);
	// A depth texture is copied whole: mip 0, slice 0
	D3D12_TEXTURE_COPY_LOCATION dst{};
	dst.pResource = image;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION srcLoc{};
	srcLoc.pResource = input->m_pResource;
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcLoc.SubresourceIndex = 0;
	list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
	D3D12_RESOURCE_BARRIER out[2] = { Dx12Transition(input->m_pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, given),
		Dx12Transition(image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_DEPTH_WRITE) };
	list->ResourceBarrier(2, out);
	list->Close();
	ID3D12CommandList* lists[] = { list };
	queue->ExecuteCommandLists(1, lists);
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	bool released = !SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage (depth)");
	const auto fenceValue = ++currentFenceValue;
	queue->Signal(frameFences[index].Get(), fenceValue);
	fenceValues[index] = fenceValue;
	return released;
}

ID3D12Resource* DX12Compositor::Resolved(const D3D12_RESOURCE_DESC& of, DXGI_FORMAT format)
{
	if (resolved) {
		D3D12_RESOURCE_DESC have = resolved->GetDesc();
		if (have.Width == of.Width && have.Height == of.Height && have.Format == format)
			return resolved.Get();
		WaitForAllFrames();
		resolved.Reset();
	}
	D3D12_RESOURCE_DESC rd = of;
	rd.Format = format;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc = { 1, 0 };
	rd.Flags = D3D12_RESOURCE_FLAG_NONE;
	D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
	if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved))))
		resolved.Reset();
	return resolved.Get();
}

void DX12Compositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	// The game's texture is in PIXEL_SHADER_RESOURCE (as OpenVR's D3D12 textures are given) and is put back there; the
	// runtime's images are in RENDER_TARGET whenever it has them
	D3D12TextureData_t* input = (D3D12TextureData_t*)texture->handle;

	D3D12_RESOURCE_DESC srcDesc;
	srcDesc = input->m_pResource->GetDesc();

	// The slice shown: SubmitWithArrayIndex's, else the first
	const UINT arrayIndex = submitSlice ? *submitSlice : 0;
	if (arrayIndex >= srcDesc.DepthOrArraySize) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("A D3D12 array texture of %u slices submitted with slice %u", (unsigned)srcDesc.DepthOrArraySize, arrayIndex);
	}

	// The bounds' area, which the swapchain is the size of
	TextureRegion region;
	if (!RegionFromBounds(bounds, (uint32_t)srcDesc.Width, srcDesc.Height, region)) {
		// Submit answers InvalidTexture; the eye is left out
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		OC_UNSUPPORTED_VOID("D3D12 texture whose bounds have no area: u %f to %f, v %f to %f", (double)Bounds(bounds).uMin, (double)Bounds(bounds).uMax, (double)Bounds(bounds).vMin, (double)Bounds(bounds).vMax);
	}

	CheckCreateSwapChain(texture, region.Width(), region.Height(), false);
	if (!chain || XR_FAILED(lastResult))
		return;

	// The shader pass converts and flips (invertUsingShaders: always used); made at its first use
	const bool flip = region.flipX || region.flipY;
	bool usePass = convertFormat || flip || oovr_global_configuration.InvertUsingShaders();
	if (usePass && !pass) {
		pass = std::make_unique<Dx12Convert>(device.Get());
		if (pass->Usable() && !pass->EnsureSlots((uint32_t)imagesHandles.size()))
			pass.reset();
	}
	if (usePass && !convertFormat && !(pass && pass->Usable())) {
		OOVR_LOG_ONCE("A D3D12 texture with flipped bounds couldn't go through the shader pass: shown as it is, not flipped");
		usePass = false;
	}

	// Reserve an image from the swapchain and wait until the compositor is done with it
	uint32_t currentIndex = 0;
	if (!AcquireAndWait((uint32_t)std::min(imagesHandles.size(), frameFences.size()), currentIndex))
		return;

	WaitForFence(frameFences[currentIndex].Get(),
	    fenceValues[currentIndex], frameFenceEvents[currentIndex]);

	commandAllocators[currentIndex]->Reset();
	auto commandList = commandLists[currentIndex].Get();
	commandList->Reset(commandAllocators[currentIndex].Get(), nullptr);

	// The area copied: no more than the bounds'
	D3D12_BOX sourceRegion{ region.x0, region.y0, 0, region.x1, region.y1, 1 };

	const D3D12_RESOURCE_STATES given = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	const bool typeless = TypedDxgi(srcDesc.Format) != srcDesc.Format;
	const bool wantSrgb = WantsSrgb(GfxApiKind::D3D12, srcDesc.Format, texture->eColorSpace);
	const DXGI_FORMAT typedFormat = !typeless ? srcDesc.Format
	    : convertFormat                       ? (DXGI_FORMAT)FormatWithSrgb(GfxApiKind::D3D12, srcDesc.Format, wantSrgb)
	                                          : (DXGI_FORMAT)createInfo.format;
	// Mip 0 of the slice
	const UINT srcSubresource = arrayIndex * srcDesc.MipLevels;
	ID3D12Resource* image = imagesHandles[currentIndex].texture;

	// A multisampled texture is resolved first; from then on that's the source, in the state the game's would be
	ID3D12Resource* from = input->m_pResource;
	UINT fromSubresource = srcSubresource;
	UINT fromSlice = arrayIndex;
	ID3D12Resource* resolvedTex = srcDesc.SampleDesc.Count > 1 ? Resolved(srcDesc, typedFormat) : nullptr;
	if (srcDesc.SampleDesc.Count > 1 && resolvedTex) {
		D3D12_RESOURCE_BARRIER in = Dx12Transition(from, given, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
		commandList->ResourceBarrier(1, &in);
		commandList->ResolveSubresource(resolvedTex, 0, from, srcSubresource, typedFormat);
		D3D12_RESOURCE_BARRIER out[2] = { Dx12Transition(from, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, given),
			Dx12Transition(resolvedTex, D3D12_RESOURCE_STATE_RESOLVE_DEST, given) };
		commandList->ResourceBarrier(2, out);
		from = resolvedTex;
		fromSubresource = 0;
		fromSlice = 0;
	}

	bool recorded = false;
	if (usePass) {
		// The area, backwards where the bounds flip it, in texture coordinates
		Dx12Convert::Pass p;
		const float w = (float)srcDesc.Width, h = (float)srcDesc.Height;
		p.offsetX = (float)(region.flipX ? region.x1 : region.x0) / w;
		p.offsetY = (float)(region.flipY ? region.y1 : region.y0) / h;
		p.scaleX = (region.flipX ? -1.0f : 1.0f) * (float)region.Width() / w;
		p.scaleY = (region.flipY ? -1.0f : 1.0f) * (float)region.Height() / h;
		const bool viewSrgb = FormatIsSrgb(GfxApiKind::D3D12, typedFormat);
		const bool targetSrgb = FormatIsSrgb(GfxApiKind::D3D12, createInfo.format);
		p.decodeSrgb = wantSrgb && !viewSrgb && targetSrgb;
		p.encodeSrgb = wantSrgb && viewSrgb && !targetSrgb;
		recorded = pass && pass->Record(commandList, currentIndex, from, typedFormat, fromSlice, image, (DXGI_FORMAT)createInfo.format,
		                       createInfo.width, createInfo.height, p);
	} else {
		D3D12_RESOURCE_BARRIER in[2] = { Dx12Transition(from, given, D3D12_RESOURCE_STATE_COPY_SOURCE),
			Dx12Transition(image, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST) };
		commandList->ResourceBarrier(2, in);
		D3D12_TEXTURE_COPY_LOCATION dst{};
		dst.pResource = image;
		dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION srcLoc{};
		srcLoc.pResource = from;
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		srcLoc.SubresourceIndex = fromSubresource;
		commandList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, &sourceRegion);
		D3D12_RESOURCE_BARRIER out[2] = { Dx12Transition(from, D3D12_RESOURCE_STATE_COPY_SOURCE, given),
			Dx12Transition(image, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET) };
		commandList->ResourceBarrier(2, out);
		recorded = true;
	}
	if (resolvedTex && from == resolvedTex) {
		D3D12_RESOURCE_BARRIER back = Dx12Transition(resolvedTex, given, D3D12_RESOURCE_STATE_RESOLVE_DEST);
		commandList->ResourceBarrier(1, &back);
	}

	commandList->Close();
	if (recorded) {
		ID3D12CommandList* set[] = { commandList };
		queue->ExecuteCommandLists(1, set);
	} else {
		// Nothing written: the eye is left out (Submit answers InvalidTexture), and the image goes back as it was
		OOVR_LOG_ONCEF("A D3D12 texture of DXGI format %d couldn't be converted into swapchain format %d", (int)srcDesc.Format, (int)createInfo.format);
		lastResult = XR_ERROR_VALIDATION_FAILURE;
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	if (recorded)
		SwapchainCallFailed(xrReleaseSwapchainImage(chain, &releaseInfo), "xrReleaseSwapchainImage");
	else
		xrReleaseSwapchainImage(chain, &releaseInfo);

	const auto fenceValue = ++currentFenceValue;
	queue->Signal(frameFences[currentIndex].Get(), fenceValue);
	fenceValues[currentIndex] = fenceValue;
}

void DX12Compositor::InvokeCubemap(const vr::Texture_t* textures)
{
	CheckCreateSwapChain(&textures[0], 0, 0, true);

	OOVR_SOFT_ABORT("TODO cubemap");
}

bool DX12Compositor::CheckChainCompatible(D3D12_RESOURCE_DESC& inputDesc, vr::EColorSpace colourSpace)
{
	bool usable = true;
#define FAIL(name)                             \
	do {                                       \
		usable = false;                        \
		OOVR_LOG("Resource mismatch: " #name); \
	} while (0);
#define CHECK(name, chainName)                  \
	if (inputDesc.name != createInfo.chainName) \
		FAIL(name);

	CHECK(Width, width)
	CHECK(Height, height)

	if (inputDesc.Format != createInfoFormat) {
		FAIL("Format");
	}
	if (colourSpace != createColourSpace) {
		FAIL("Colour space");
	}

	// CHECK_ADV(SampleDesc.Count, SampleCount);
	// CHECK_ADV(SampleDesc.Quality);
#undef CHECK
#undef FAIL

	return usable;
}

#endif
