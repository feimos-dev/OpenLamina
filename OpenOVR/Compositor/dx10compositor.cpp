#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX10) && defined(SUPPORT_DX11)

#include "dx10compositor.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

// How long either side waits for the other's key: a copy takes far less, so a longer wait is a stuck texture
constexpr DWORD kSyncMs = 100;

// More textures than a game submits in turn (its swapchain's, a few per eye): the copies are dropped and made again
constexpr size_t kMaxShared = 16;

std::mutex bridgeLock;
ComPtr<ID3D10Device> bridgeFor;
ComPtr<ID3D11Device> bridge;

// A typed format a multisampled texture of this format is resolved to (ResolveSubresource needs one)
DXGI_FORMAT ResolveFormat(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return format;
	}
}

// The format of the shared copy: the typeless one of the game's format's group, as Direct3D 10 shares no sRGB formats
// (a copy between formats of one group keeps the bits, and DX11Compositor reads a typeless 8-bit format as gamma under
// ColorSpace_Auto, as it reads an sRGB one)
DXGI_FORMAT ShareableFormat(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8X8_TYPELESS;
	default:
		return format;
	}
}

// A texture of the device, for DX11Compositor's constructor (which takes the device from a texture)
ComPtr<ID3D11Texture2D> OneTexel(ID3D11Device* device)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = desc.Height = 1;
	desc.MipLevels = desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Texture2D> tex;
	OOVR_FAILED_DX_ABORT(device->CreateTexture2D(&desc, nullptr, &tex));
	return tex;
}

} // namespace

DX10Compositor::DX10Compositor(ID3D10Texture2D* initial)
    : DX11Compositor(OneTexel(BridgeDevice(initial)).Get())
{
}

ID3D10Texture2D* DX10Compositor::AsD3D10Texture(const vr::Texture_t* texture)
{
	if (!texture || texture->eType != vr::TextureType_DirectX || !texture->handle)
		return nullptr;
	// Both kinds answer both interfaces. The game hands over its own API's: a D3D11 texture's handle is its
	// ID3D11Texture2D (whose ID3D10Texture2D can have a device too, once anything has used D3D10's interfaces on its
	// device). Otherwise only a D3D10 texture's ID3D10Texture2D has a device.
	auto* unknown = (IUnknown*)texture->handle;
	ComPtr<ID3D11Texture2D> tex11;
	if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&tex11))) && (void*)tex11.Get() == texture->handle)
		return nullptr;
	ComPtr<ID3D10Texture2D> tex10;
	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&tex10))))
		return nullptr;
	ComPtr<ID3D10Device> device10;
	tex10->GetDevice(&device10);
	if (!device10)
		return nullptr;
	return tex10.Get(); // the game's reference keeps it
}

ID3D11Device* DX10Compositor::BridgeDevice(ID3D10Texture2D* tex)
{
	ComPtr<ID3D10Device> device10;
	tex->GetDevice(&device10);
	if (!device10)
		return nullptr;

	std::lock_guard<std::mutex> lock(bridgeLock);
	if (bridge && bridgeFor == device10)
		return bridge.Get();

	ComPtr<IDXGIDevice> dxgi;
	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(device10.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter))) {
		OOVR_LOG("Direct3D 10: the game's device has no DXGI adapter");
		return nullptr;
	}
	DXGI_ADAPTER_DESC adapterDesc{};
	adapter->GetDesc(&adapterDesc);

	// The same adapter through a DXGI 1.1 factory, as a D3D10 device's own (DXGI 1.0) adapter makes a D3D11 device
	// that can't share textures with keyed mutexes
	ComPtr<IDXGIAdapter> sameAdapter = adapter;
	ComPtr<IDXGIFactory1> factory;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
		ComPtr<IDXGIAdapter1> candidate;
		// Bounded by the adapter count, which EnumAdapters1 ends with DXGI_ERROR_NOT_FOUND
		for (UINT i = 0; factory->EnumAdapters1(i, &candidate) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 desc{};
			if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == adapterDesc.AdapterLuid.LowPart
			    && desc.AdapterLuid.HighPart == adapterDesc.AdapterLuid.HighPart) {
				sameAdapter = candidate;
				break;
			}
			candidate.Reset();
		}
	}

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	ComPtr<ID3D11Device> device11;
	HRESULT hr = D3D11CreateDevice(sameAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, (UINT)std::size(levels),
	    D3D11_SDK_VERSION, &device11, nullptr, nullptr);
	if (hr == E_INVALIDARG) // no 11.1 on this system
		hr = D3D11CreateDevice(sameAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, (UINT)std::size(levels) - 1,
		    D3D11_SDK_VERSION, &device11, nullptr, nullptr);
	if (FAILED(hr)) {
		OOVR_LOGF("Direct3D 10: no Direct3D 11 device on the game's adapter (%ls): 0x%08lx", adapterDesc.Description, (unsigned long)hr);
		return nullptr;
	}
	OOVR_LOGF("Direct3D 10: the game's frames go through a Direct3D 11 device on %ls", adapterDesc.Description);
	bridgeFor = device10;
	bridge = device11;
	return bridge.Get();
}

void DX10Compositor::ReleaseBridgeDevice()
{
	std::lock_guard<std::mutex> lock(bridgeLock);
	bridge.Reset();
	bridgeFor.Reset();
}

namespace {

// Waits (at most kSyncMs) until the GPU has done what the device was asked to do: for a texture shared without a keyed
// mutex
bool WaitForGpu10(ID3D10Device* device)
{
	D3D10_QUERY_DESC desc{ D3D10_QUERY_EVENT, 0 };
	ComPtr<ID3D10Query> query;
	if (FAILED(device->CreateQuery(&desc, &query)))
		return false;
	query->End();
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSyncMs);
	BOOL done = FALSE;
	while (true) {
		HRESULT hr = query->GetData(&done, sizeof(done), 0);
		if (hr == S_OK)
			return true;
		if (FAILED(hr) || std::chrono::steady_clock::now() > until)
			return false;
		std::this_thread::yield();
	}
}

bool WaitForGpu11(ID3D11Device* device, ID3D11DeviceContext* context)
{
	D3D11_QUERY_DESC desc{ D3D11_QUERY_EVENT, 0 };
	ComPtr<ID3D11Query> query;
	if (FAILED(device->CreateQuery(&desc, &query)))
		return false;
	context->End(query.Get());
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSyncMs);
	BOOL done = FALSE;
	while (true) {
		HRESULT hr = context->GetData(query.Get(), &done, sizeof(done), 0);
		if (hr == S_OK)
			return true;
		if (FAILED(hr) || std::chrono::steady_clock::now() > until)
			return false;
		std::this_thread::yield();
	}
}

} // namespace

DX10Compositor::Shared* DX10Compositor::SharedFor(ID3D10Texture2D* game)
{
	D3D10_TEXTURE2D_DESC desc{};
	game->GetDesc(&desc);
	auto it = shared.find(game);
	if (it != shared.end() && memcmp(&it->second.gameDesc, &desc, sizeof(desc)) == 0)
		return &it->second;
	if (it != shared.end())
		shared.erase(it);
	if (shared.size() >= kMaxShared)
		shared.clear();

	ComPtr<ID3D10Device> device10;
	game->GetDevice(&device10);

	// The game's first slice and mip, resolved if it's multisampled
	D3D10_TEXTURE2D_DESC sharedDesc{};
	sharedDesc.Width = desc.Width;
	sharedDesc.Height = desc.Height;
	sharedDesc.MipLevels = 1;
	sharedDesc.ArraySize = 1;
	sharedDesc.Format = ShareableFormat(desc.SampleDesc.Count > 1 ? ResolveFormat(desc.Format) : desc.Format);
	sharedDesc.SampleDesc.Count = 1;
	sharedDesc.Usage = D3D10_USAGE_DEFAULT;
	sharedDesc.BindFlags = D3D10_BIND_SHADER_RESOURCE | D3D10_BIND_RENDER_TARGET;

	Shared s;
	s.gameDesc = desc;
	// With a keyed mutex where the game's device can share one (a device of a DXGI 1.1 factory's adapter). A D3D10
	// device made on the default adapter can't: then a plain shared texture, and each side waits for its copy.
	HRESULT hr = E_POINTER;
	if (device10) {
		sharedDesc.MiscFlags = D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		hr = device10->CreateTexture2D(&sharedDesc, nullptr, &s.tex10);
		if (FAILED(hr)) {
			sharedDesc.MiscFlags = D3D10_RESOURCE_MISC_SHARED;
			hr = device10->CreateTexture2D(&sharedDesc, nullptr, &s.tex10);
		}
	}
	ComPtr<IDXGIResource> resource;
	HANDLE handle = nullptr;
	if (SUCCEEDED(hr))
		hr = s.tex10.As(&resource);
	if (SUCCEEDED(hr))
		hr = resource->GetSharedHandle(&handle);
	if (SUCCEEDED(hr))
		hr = device->OpenSharedResource(handle, IID_PPV_ARGS(&s.tex11));
	if (SUCCEEDED(hr) && sharedDesc.MiscFlags == D3D10_RESOURCE_MISC_SHARED_KEYEDMUTEX) {
		hr = s.tex10.As(&s.mutex10);
		if (SUCCEEDED(hr))
			hr = s.tex11.As(&s.mutex11);
	}
	if (FAILED(hr))
		OC_UNSUPPORTED(nullptr, "Direct3D 10: no shared copy of a %ux%u texture of DXGI format %d (0x%08lx): it isn't shown",
		    desc.Width, desc.Height, (int)desc.Format, (unsigned long)hr);
	return &(shared[game] = std::move(s));
}

void DX10Compositor::CopyToSwapchain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, std::optional<XruEye> eye, vr::EVRSubmitFlags submitFlags)
{
	ID3D10Texture2D* game = AsD3D10Texture(texture);
	if (!game) {
		OOVR_LOG_ONCE("Direct3D 10: a texture that isn't a D3D10 one, submitted after D3D10 ones: left out");
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		return;
	}
	Shared* s = SharedFor(game);
	if (!s) {
		lastResult = XR_ERROR_VALIDATION_FAILURE;
		return;
	}

	// The D3D10 side: the game's texture into the shared one (key 0 to 1)
	ComPtr<ID3D10Device> device10;
	game->GetDevice(&device10);
	if (s->mutex10 && s->mutex10->AcquireSync(0, kSyncMs) != S_OK) {
		OOVR_LOG_ONCE("Direct3D 10: the shared copy stayed locked: frame left out");
		shared.erase(game); // made again next time
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	if (s->gameDesc.SampleDesc.Count > 1)
		device10->ResolveSubresource(s->tex10.Get(), 0, game, 0, ResolveFormat(s->gameDesc.Format));
	else if (s->gameDesc.MipLevels == 1 && s->gameDesc.ArraySize == 1)
		device10->CopyResource(s->tex10.Get(), game);
	else
		device10->CopySubresourceRegion(s->tex10.Get(), 0, 0, 0, 0, game, 0, nullptr);
	if (s->mutex10) {
		s->mutex10->ReleaseSync(1);
	} else if (!WaitForGpu10(device10.Get())) {
		OOVR_LOG_ONCE("Direct3D 10: the game's device didn't finish the copy in time: frame left out");
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}

	// The D3D11 side: as a D3D11 texture (key 1 to 0)
	if (s->mutex11 && s->mutex11->AcquireSync(1, kSyncMs) != S_OK) {
		OOVR_LOG_ONCE("Direct3D 10: the game's copy didn't reach the Direct3D 11 side: frame left out");
		shared.erase(game); // its key is lost: made again next time
		lastResult = XR_ERROR_RUNTIME_FAILURE;
		return;
	}
	vr::Texture_t copy = *texture;
	copy.handle = s->tex11.Get();
	DX11Compositor::CopyToSwapchain(&copy, bounds, eye, submitFlags);
	if (s->mutex11)
		s->mutex11->ReleaseSync(0);
	else if (!WaitForGpu11(device, context)) // before the game's next copy overwrites it
		OOVR_LOG_ONCE("Direct3D 10: the copy into the swapchain didn't finish in time");
}

#endif
