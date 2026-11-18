#include "OverlayTextureCopy.h"

#include "GfxInterop.h"

#if defined(_WIN32) && defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#ifdef SUPPORT_DX12
#include <d3d12.h>
#endif

#include <chrono>
#include <map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// More textures than an overlay program shows in turn: the copies are dropped and made again
constexpr size_t kMaxCopies = 16;
// How long a copy may take before it's given up on (the program's GPU is busy), in milliseconds
constexpr DWORD kCopyTimeoutMs = 200;

uint64_t PackLuid(const LUID& luid)
{
	return (uint64_t)luid.LowPart | ((uint64_t)(uint32_t)luid.HighPart << 32);
}

// The adapter a DXGI device is on, 0 if it can't be told
uint64_t AdapterOf(IUnknown* device)
{
	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc{};
	if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
		return 0;
	return PackLuid(desc.AdapterLuid);
}

bool IsTypeless(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
		return true;
	default:
		return false;
	}
}

} // namespace

struct OverlayTextureCopy::Impl {
	ID3D11Device* device = nullptr;
	ComPtr<ID3D11Device1> device1;
	ComPtr<ID3D11DeviceContext> context;
	uint64_t adapter = 0;

	// A D3D11 texture of the program's, through a shared texture of its device
	struct D3D11Copy {
		ComPtr<ID3D11Device> programDevice;
		D3D11_TEXTURE2D_DESC source{};
		ComPtr<ID3D11Texture2D> programSide; // the program's device's, shared
		ComPtr<IDXGIKeyedMutex> programMutex;
		ComPtr<ID3D11Texture2D> opened; // the same texture, on the device
		ComPtr<IDXGIKeyedMutex> openedMutex;
		ComPtr<ID3D11Texture2D> plain; // the device's own copy, which the compositor reads
	};
	std::map<void*, D3D11Copy> d3d11;

#ifdef SUPPORT_DX12
	// A D3D12 resource of the program's, through a shared resource of its device
	struct D3D12Copy {
		ComPtr<ID3D12Device> programDevice;
		D3D12_RESOURCE_DESC source{};
		ComPtr<ID3D12Resource> shared;
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		uint64_t fenceValue = 0;
		HANDLE event = nullptr;
		ComPtr<ID3D11Texture2D> opened;
		ComPtr<ID3D11Texture2D> plain;
		// Ended after the device's read of opened: the next copy writes shared only once it's done
		ComPtr<ID3D11Query> readDone;
		bool readPending = false;
		bool Running() const { return fence && fence->GetCompletedValue() < fenceValue; }
		~D3D12Copy()
		{
			// Waited for (bounded) while the program's queue still runs its copy; past that, its objects are left to it
			if (Running() && event && SUCCEEDED(fence->SetEventOnCompletion(fenceValue, event)))
				WaitForSingleObject(event, kCopyTimeoutMs);
			if (Running()) {
				allocator.Detach();
				list.Detach();
				shared.Detach();
			}
			if (event)
				CloseHandle(event);
		}
		D3D12Copy() = default;
		D3D12Copy(const D3D12Copy&) = delete;
		D3D12Copy& operator=(const D3D12Copy&) = delete;
	};
	std::map<void*, std::unique_ptr<D3D12Copy>> d3d12;
	// Dropped copies whose last copy was still running, released once it's done
	std::vector<std::unique_ptr<D3D12Copy>> retired;

	void Retire(std::unique_ptr<D3D12Copy> copy)
	{
		if (!copy->Running())
			return;
		retired.push_back(std::move(copy));
		// Past the bound, the oldest goes now (its destructor waits for it, bounded)
		if (retired.size() > kMaxCopies)
			retired.erase(retired.begin());
	}

	void ReleaseFinished()
	{
		std::erase_if(retired, [](const std::unique_ptr<D3D12Copy>& c) { return !c->Running(); });
	}

	// Whether the device has done its read of the copy's shared texture, waited for at most kCopyTimeoutMs
	bool ReadDone(D3D12Copy& copy)
	{
		if (!copy.readPending)
			return true;
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCopyTimeoutMs);
		HRESULT hr = S_FALSE;
		while ((hr = context->GetData(copy.readDone.Get(), nullptr, 0, 0)) == S_FALSE && std::chrono::steady_clock::now() < until)
			Sleep(1);
		copy.readPending = hr != S_OK;
		return hr == S_OK;
	}
#endif

	// OpenGL and Vulkan: one each, for the program's context or device
	std::unique_ptr<GfxInterop> gl;
#ifdef SUPPORT_VK
	std::unique_ptr<GfxInterop> vk;
	VkDevice vkDevice = VK_NULL_HANDLE;
#endif

	// The device's own texture of desc (no sharing, one layer), nullptr if it can't be made
	ComPtr<ID3D11Texture2D> Plain(const D3D11_TEXTURE2D_DESC& desc)
	{
		D3D11_TEXTURE2D_DESC d = desc;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.SampleDesc = { 1, 0 };
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		d.CPUAccessFlags = 0;
		d.MiscFlags = 0;
		ComPtr<ID3D11Texture2D> tex;
		if (FAILED(device->CreateTexture2D(&d, nullptr, &tex)))
			return nullptr;
		return tex;
	}

	ID3D11Texture2D* FromD3D11(ID3D11Texture2D* src, std::string* why)
	{
		ComPtr<ID3D11Device> programDevice;
		src->GetDevice(&programDevice);
		if (!programDevice) {
			*why = "the D3D11 texture has no device";
			return nullptr;
		}
		if (AdapterOf(programDevice.Get()) != adapter) {
			*why = "the D3D11 texture is on another graphics adapter than the runtime's";
			return nullptr;
		}
		D3D11_TEXTURE2D_DESC sd{};
		src->GetDesc(&sd);
		if (sd.SampleDesc.Count > 1 && IsTypeless(sd.Format)) {
			*why = "a multisampled typeless D3D11 texture can't be resolved for copying";
			return nullptr;
		}

		auto found = d3d11.find(src);
		if (found != d3d11.end()) {
			const D3D11_TEXTURE2D_DESC& was = found->second.source;
			if (found->second.programDevice != programDevice || was.Width != sd.Width || was.Height != sd.Height || was.Format != sd.Format
			    || was.SampleDesc.Count != sd.SampleDesc.Count)
				d3d11.erase(found); // another texture where an old one was
		}
		D3D11Copy* copy = nullptr;
		if (auto it = d3d11.find(src); it != d3d11.end()) {
			copy = &it->second;
		} else {
			if (d3d11.size() >= kMaxCopies)
				d3d11.clear();
			D3D11Copy made;
			made.programDevice = programDevice;
			made.source = sd;
			D3D11_TEXTURE2D_DESC shared = sd;
			shared.MipLevels = 1;
			shared.ArraySize = 1;
			shared.SampleDesc = { 1, 0 };
			shared.Usage = D3D11_USAGE_DEFAULT;
			shared.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			shared.CPUAccessFlags = 0;
			shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
			ComPtr<IDXGIResource1> resource;
			HANDLE handle = nullptr;
			if (FAILED(programDevice->CreateTexture2D(&shared, nullptr, &made.programSide)) || FAILED(made.programSide.As(&resource))
			    || FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
				*why = "no shared D3D11 texture of its format on the program's device";
				return nullptr;
			}
			const HRESULT opened = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&made.opened));
			CloseHandle(handle);
			if (FAILED(opened) || FAILED(made.programSide.As(&made.programMutex)) || FAILED(made.opened.As(&made.openedMutex))) {
				*why = "the shared D3D11 texture can't be opened on OpenLamina's device";
				return nullptr;
			}
			made.plain = Plain(shared);
			if (!made.plain) {
				*why = "no D3D11 texture of its format on OpenLamina's device";
				return nullptr;
			}
			copy = &d3d11.emplace(src, std::move(made)).first->second;
		}

		// On the program's device (its thread: the program is in the call), then on the device
		ComPtr<ID3D11DeviceContext> programContext;
		programDevice->GetImmediateContext(&programContext);
		if (copy->programMutex->AcquireSync(0, kCopyTimeoutMs) != S_OK) {
			*why = "the shared texture stayed in use";
			d3d11.erase(src); // a new shared pair next time
			return nullptr;
		}
		if (sd.SampleDesc.Count > 1)
			programContext->ResolveSubresource(copy->programSide.Get(), 0, src, 0, sd.Format);
		else
			programContext->CopySubresourceRegion(copy->programSide.Get(), 0, 0, 0, 0, src, D3D11CalcSubresource(0, 0, sd.MipLevels), nullptr);
		copy->programMutex->ReleaseSync(1);
		if (copy->openedMutex->AcquireSync(1, kCopyTimeoutMs) != S_OK) {
			*why = "the program's copy didn't finish in time";
			// The keyed mutex stays at the program's key, with nothing to hand it back: a new shared pair next time
			d3d11.erase(src);
			return nullptr;
		}
		context->CopyResource(copy->plain.Get(), copy->opened.Get());
		copy->openedMutex->ReleaseSync(0);
		return copy->plain.Get();
	}

#ifdef SUPPORT_DX12
	ID3D11Texture2D* FromD3D12(const vr::D3D12TextureData_t* data, std::string* why)
	{
		if (!data || !data->m_pResource || !data->m_pCommandQueue) {
			*why = "the D3D12 texture data has no resource or queue";
			return nullptr;
		}
		ReleaseFinished();
		ID3D12Resource* src = data->m_pResource;
		ComPtr<ID3D12Device> programDevice;
		src->GetDevice(IID_PPV_ARGS(&programDevice));
		if (!programDevice || PackLuid(programDevice->GetAdapterLuid()) != adapter) {
			*why = "the D3D12 resource is on another graphics adapter than the runtime's";
			return nullptr;
		}
		const D3D12_RESOURCE_DESC sd = src->GetDesc();
		if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || (sd.SampleDesc.Count > 1 && IsTypeless(sd.Format))) {
			*why = "the D3D12 resource isn't a 2D texture that can be copied";
			return nullptr;
		}

		auto found = d3d12.find(src);
		if (found != d3d12.end()) {
			const D3D12_RESOURCE_DESC& was = found->second->source;
			if (found->second->programDevice != programDevice || was.Width != sd.Width || was.Height != sd.Height || was.Format != sd.Format
			    || was.SampleDesc.Count != sd.SampleDesc.Count) {
				Retire(std::move(found->second));
				d3d12.erase(found);
			}
		}
		D3D12Copy* copy = nullptr;
		if (auto it = d3d12.find(src); it != d3d12.end()) {
			copy = it->second.get();
		} else {
			if (d3d12.size() >= kMaxCopies) {
				for (auto& kv : d3d12)
					Retire(std::move(kv.second));
				d3d12.clear();
			}
			auto made = std::make_unique<D3D12Copy>();
			made->programDevice = programDevice;
			made->source = sd;
			D3D12_RESOURCE_DESC shared = sd;
			shared.MipLevels = 1;
			shared.DepthOrArraySize = 1;
			shared.SampleDesc = { 1, 0 };
			shared.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			// D3D11 opens a shared texture that may be used by both at once: simultaneous access, which a format that
			// can't be rendered to may lack (then render target use too)
			HRESULT opened = E_FAIL;
			for (D3D12_RESOURCE_FLAGS flags : { D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS,
			         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, D3D12_RESOURCE_FLAG_NONE }) {
				shared.Flags = flags;
				made->shared.Reset();
				HANDLE handle = nullptr;
				if (FAILED(programDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &shared, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&made->shared)))
				    || FAILED(programDevice->CreateSharedHandle(made->shared.Get(), nullptr, GENERIC_ALL, nullptr, &handle)))
					continue;
				opened = device1->OpenSharedResource1(handle, IID_PPV_ARGS(&made->opened));
				CloseHandle(handle);
				if (SUCCEEDED(opened))
					break;
			}
			if (FAILED(opened)) {
				*why = "no D3D12 resource of its format could be shared with OpenLamina's D3D11 device";
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC od{};
			made->opened->GetDesc(&od);
			made->plain = Plain(od);
			made->event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
			const D3D11_QUERY_DESC eventQuery{ D3D11_QUERY_EVENT, 0 };
			if (!made->plain || !made->event
			    || FAILED(programDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&made->allocator)))
			    || FAILED(programDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made->allocator.Get(), nullptr, IID_PPV_ARGS(&made->list)))
			    || FAILED(made->list->Close()) || FAILED(programDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made->fence)))
			    || FAILED(device->CreateQuery(&eventQuery, &made->readDone))) {
				*why = "no D3D12 copy objects on the program's device";
				return nullptr;
			}
			copy = d3d12.emplace(src, std::move(made)).first->second.get();
		}

		// On the program's queue: the program's texture is in PIXEL_SHADER_RESOURCE, as OpenVR's D3D12 textures are
		// given, and is put back there; the shared one is left in COMMON for D3D11
		const D3D12_RESOURCE_STATES given = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		auto transition = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER b{};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = r;
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = before;
			b.Transition.StateAfter = after;
			return b;
		};
		// Its objects are reset only once the program's queue is done with them, and shared written only once the
		// device has read it
		if (copy->Running()) {
			*why = "the last D3D12 copy is still running";
			return nullptr;
		}
		if (!ReadDone(*copy)) {
			*why = "OpenLamina's device is still reading the last D3D12 copy";
			return nullptr;
		}
		if (FAILED(copy->allocator->Reset()) || FAILED(copy->list->Reset(copy->allocator.Get(), nullptr))) {
			*why = "the D3D12 copy's command list can't be reset";
			return nullptr;
		}
		ID3D12GraphicsCommandList* list = copy->list.Get();
		if (sd.SampleDesc.Count > 1) {
			D3D12_RESOURCE_BARRIER to[2] = { transition(src, given, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
				transition(copy->shared.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_DEST) };
			list->ResourceBarrier(2, to);
			list->ResolveSubresource(copy->shared.Get(), 0, src, 0, sd.Format);
			D3D12_RESOURCE_BARRIER back[2] = { transition(src, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, given),
				transition(copy->shared.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COMMON) };
			list->ResourceBarrier(2, back);
		} else {
			D3D12_RESOURCE_BARRIER to[2] = { transition(src, given, D3D12_RESOURCE_STATE_COPY_SOURCE),
				transition(copy->shared.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
			list->ResourceBarrier(2, to);
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = copy->shared.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			dst.SubresourceIndex = 0;
			D3D12_TEXTURE_COPY_LOCATION from{};
			from.pResource = src;
			from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			from.SubresourceIndex = 0;
			list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
			D3D12_RESOURCE_BARRIER back[2] = { transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, given),
				transition(copy->shared.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON) };
			list->ResourceBarrier(2, back);
		}
		if (FAILED(list->Close())) {
			*why = "the D3D12 copy's command list can't be closed";
			return nullptr;
		}
		ID3D12CommandList* lists[] = { list };
		data->m_pCommandQueue->ExecuteCommandLists(1, lists);
		const uint64_t value = ++copy->fenceValue;
		if (FAILED(data->m_pCommandQueue->Signal(copy->fence.Get(), value)) || FAILED(copy->fence->SetEventOnCompletion(value, copy->event))
		    || WaitForSingleObject(copy->event, kCopyTimeoutMs) != WAIT_OBJECT_0) {
			*why = "the D3D12 copy didn't finish in time";
			// Still running: the next copy is refused until its fence has passed
			return nullptr;
		}
		context->CopyResource(copy->plain.Get(), copy->opened.Get());
		context->End(copy->readDone.Get());
		copy->readPending = true;
		return copy->plain.Get();
	}
#endif
};

OverlayTextureCopy::OverlayTextureCopy(ID3D11Device* onDevice)
    : impl(std::make_unique<Impl>()), device(onDevice)
{
	if (device)
		device->AddRef();
	impl->device = device;
	if (device) {
		device->QueryInterface(IID_PPV_ARGS(&impl->device1));
		device->GetImmediateContext(&impl->context);
		impl->adapter = AdapterOf(device);
	}
}

OverlayTextureCopy::~OverlayTextureCopy()
{
	impl.reset();
	if (device)
		device->Release();
}

ID3D11Texture2D* OverlayTextureCopy::CopyIn(const vr::Texture_t* tex, const vr::VRTextureBounds_t* bounds, std::string* why)
{
	std::string ignored;
	if (!why)
		why = &ignored;
	if (!tex || !tex->handle) {
		*why = "no texture";
		return nullptr;
	}
	if (!device || !impl->device1 || !impl->context) {
		*why = "OpenLamina's D3D11 device can't open shared textures";
		return nullptr;
	}
	switch (tex->eType) {
	case vr::TextureType_DirectX: {
		// A Direct3D 10 texture answers as one too, but its device isn't a D3D11 device
		ComPtr<ID3D11Texture2D> d3d11;
		if (FAILED(((IUnknown*)tex->handle)->QueryInterface(IID_PPV_ARGS(&d3d11)))) {
			*why = "it isn't a D3D11 texture";
			return nullptr;
		}
		return impl->FromD3D11(d3d11.Get(), why);
	}
#ifdef SUPPORT_DX12
	case vr::TextureType_DirectX12:
		return impl->FromD3D12((const vr::D3D12TextureData_t*)tex->handle, why);
#endif
#ifdef SUPPORT_GL
	case vr::TextureType_OpenGL:
		if (!impl->gl)
			impl->gl = GfxInterop::ForGL(device);
		if (!impl->gl) {
			*why = "the program's OpenGL can't share textures with D3D11 (WGL_NV_DX_interop2)";
			return nullptr;
		}
		if (ID3D11Texture2D* copied = impl->gl->CopyIn(tex, bounds))
			return copied;
		*why = "the OpenGL texture couldn't be copied";
		return nullptr;
#endif
#ifdef SUPPORT_VK
	case vr::TextureType_Vulkan: {
		const auto* vktex = (const vr::VRVulkanTextureData_t*)tex->handle;
		if (!impl->vk || impl->vkDevice != vktex->m_pDevice) {
			impl->vk.reset();
			impl->vk = GfxInterop::ForVulkan(vktex->m_pInstance, vktex->m_pPhysicalDevice, vktex->m_pDevice, vktex->m_pQueue, device);
			impl->vkDevice = impl->vk ? vktex->m_pDevice : VK_NULL_HANDLE;
		}
		if (!impl->vk) {
			*why = "the program's Vulkan device can't share textures with D3D11 (VK_KHR_external_memory_win32, the runtime's adapter)";
			return nullptr;
		}
		if (ID3D11Texture2D* copied = impl->vk->CopyIn(tex, bounds))
			return copied;
		*why = "the Vulkan texture couldn't be copied";
		return nullptr;
	}
#endif
	default:
		*why = "textures of this type can't be copied to OpenLamina's device";
		return nullptr;
	}
}

#else // without D3D11: no texture can be copied

struct OverlayTextureCopy::Impl {
};

OverlayTextureCopy::OverlayTextureCopy(ID3D11Device* onDevice)
    : device(onDevice)
{
}

OverlayTextureCopy::~OverlayTextureCopy() = default;

ID3D11Texture2D* OverlayTextureCopy::CopyIn(const vr::Texture_t*, const vr::VRTextureBounds_t*, std::string* why)
{
	if (why)
		*why = "textures are copied through D3D11, which this platform lacks";
	return nullptr;
}

#endif
