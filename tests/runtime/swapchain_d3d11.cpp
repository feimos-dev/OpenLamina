// The stand-in runtime's D3D11 sessions (XR_KHR_D3D11_enable): swapchain images made on the app's device, read back
// through a staging copy on its immediate context.

#include "runtime_internal.h"

#include "probe.h"

#include <dxgi1_2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

bool RtRuntimeAdapterLuid(LUID* out)
{
	char want[64] = {};
	DWORD len = GetEnvironmentVariableA("OC_TEST_RUNTIME_GPU_LUID", want, sizeof(want));
	unsigned long long wanted = 0;
	bool named = len > 0 && len < sizeof(want) && sscanf_s(want, "%llx", &wanted) == 1;
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
		return false;
	bool found = false;
	IDXGIAdapter1* adapter = nullptr;
	// Bounded by the adapter count (EnumAdapters1 ends with DXGI_ERROR_NOT_FOUND)
	for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);
		adapter->Release();
		unsigned long long luid = ((unsigned long long)(uint32_t)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;
		if (i == 0 || (named && luid == wanted)) {
			*out = desc.AdapterLuid;
			found = true;
		}
		if (named && luid == wanted)
			break;
	}
	factory->Release();
	return found;
}

namespace {

XrResult XRAPI_CALL GetD3D11GraphicsRequirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D11KHR* reqs)
{
	XrResult injected;
	if (ScriptFails("xrGetD3D11GraphicsRequirementsKHR", &injected))
		return injected;
	{
		std::lock_guard<std::recursive_mutex> g(St().lock);
		St().requirementsQueried.insert(RtApi::D3D11);
	}
	if (!RtRuntimeAdapterLuid(&reqs->adapterLuid))
		return XR_ERROR_RUNTIME_FAILURE;
	reqs->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
	std::lock_guard<std::recursive_mutex> g(St().lock);
	St().requiredLuid = reqs->adapterLuid;
	return XR_SUCCESS;
}

XrResult Bind(const XrBaseInStructure* binding, RtSession* s)
{
	auto* b = (const XrGraphicsBindingD3D11KHR*)binding;
	if (!b->device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	// The device must be on the adapter the requirements named (Monado: XR_ERROR_GRAPHICS_DEVICE_INVALID)
	IDXGIDevice* dxgi = nullptr;
	IDXGIAdapter* adapter = nullptr;
	DXGI_ADAPTER_DESC desc{};
	bool known = SUCCEEDED(b->device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi))
	    && SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc));
	if (adapter)
		adapter->Release();
	if (dxgi)
		dxgi->Release();
	if (!known || std::memcmp(&desc.AdapterLuid, &St().requiredLuid, sizeof(LUID)) != 0)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	s->device = b->device;
	s->device->AddRef();
	return XR_SUCCESS;
}

void Unbind(RtSession* s)
{
	if (s->device)
		s->device->Release();
	s->device = nullptr;
}

const std::vector<int64_t>& Formats()
{
	static const std::vector<int64_t> formats = {
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
		DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		DXGI_FORMAT_R16G16B16A16_FLOAT,
		DXGI_FORMAT_D32_FLOAT,
	};
	return formats;
}

const std::vector<int64_t>& DefaultFormats()
{
	static const std::vector<int64_t> formats = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
		DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
	return formats;
}

void Destroy(RtSession*, RtSwapchain* sc)
{
	for (uint64_t tex : sc->images)
		((ID3D11Texture2D*)(uintptr_t)tex)->Release();
	sc->images.clear();
}

XrResult Create(RtSession* session, const XrSwapchainCreateInfo* info, RtSwapchain* sc)
{
	if (!session->device)
		return XR_ERROR_VALIDATION_FAILURE;
	if (std::find(Formats().begin(), Formats().end(), info->format) == Formats().end())
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;

	bool depth = info->format == DXGI_FORMAT_D32_FLOAT;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = info->width;
	desc.Height = info->height;
	desc.MipLevels = info->mipCount ? info->mipCount : 1;
	desc.ArraySize = (info->arraySize ? info->arraySize : 1) * (info->faceCount ? info->faceCount : 1);
	desc.Format = (DXGI_FORMAT)info->format;
	desc.SampleDesc.Count = info->sampleCount ? info->sampleCount : 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = depth ? D3D11_BIND_DEPTH_STENCIL : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

	sc->info = *info;
	for (int i = 0; i < 3; i++) {
		ID3D11Texture2D* tex = nullptr;
		if (FAILED(session->device->CreateTexture2D(&desc, nullptr, &tex))) {
			Destroy(session, sc);
			return XR_ERROR_RUNTIME_FAILURE;
		}
		sc->images.push_back((uint64_t)(uintptr_t)tex);
	}
	return XR_SUCCESS;
}

void Enumerate(const RtSwapchain* sc, XrSwapchainImageBaseHeader* images)
{
	auto* out = (XrSwapchainImageD3D11KHR*)images;
	for (size_t i = 0; i < sc->images.size(); i++)
		out[i].texture = (ID3D11Texture2D*)(uintptr_t)sc->images[i];
}

bool Read(RtSession* session, RtSwapchain* sc, uint32_t index, uint32_t arrayIndex,
    const std::vector<XrOffset2Di>& points, uint32_t bytesPerTexel, std::vector<uint8_t>& out, std::string* why)
{
	*why = "the D3D11 readback failed";
	if (!session->device || index >= sc->images.size() || points.empty())
		return false;
	auto* tex = (ID3D11Texture2D*)(uintptr_t)sc->images[index];
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	if (arrayIndex >= desc.ArraySize)
		return false;
	ID3D11DeviceContext* ctx = nullptr;
	session->device->GetImmediateContext(&ctx);
	if (!ctx)
		return false;

	// A depth image is copied whole (D3D11 copies no part of one) into a staging texture of its typeless family
	if (LayoutOfDxgi(desc.Format) == PixelLayout::DepthF32) {
		D3D11_TEXTURE2D_DESC sd = desc;
		sd.Format = DXGI_FORMAT_R32_TYPELESS;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		ID3D11Texture2D* staging = nullptr;
		bool ok = desc.SampleDesc.Count == 1 && SUCCEEDED(session->device->CreateTexture2D(&sd, nullptr, &staging));
		if (ok) {
			ctx->CopyResource(staging, tex);
			UINT sub = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
			D3D11_MAPPED_SUBRESOURCE mapped;
			ok = SUCCEEDED(ctx->Map(staging, sub, D3D11_MAP_READ, 0, &mapped));
			if (ok) {
				out.clear();
				for (const XrOffset2Di& p : points) {
					UINT x = (UINT)std::min<int32_t>(std::max<int32_t>(p.x, 0), (int32_t)desc.Width - 1);
					UINT y = (UINT)std::min<int32_t>(std::max<int32_t>(p.y, 0), (int32_t)desc.Height - 1);
					const uint8_t* at = (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch + (size_t)x * bytesPerTexel;
					out.insert(out.end(), at, at + bytesPerTexel);
				}
				ctx->Unmap(staging, sub);
			}
			staging->Release();
		}
		ctx->Release();
		return ok;
	}

	// A multisampled image is resolved first: only single-sampled regions can be copied
	ID3D11Texture2D* src = tex;
	UINT srcSub = D3D11CalcSubresource(0, arrayIndex, desc.MipLevels);
	ID3D11Texture2D* resolved = nullptr;
	if (desc.SampleDesc.Count > 1) {
		D3D11_TEXTURE2D_DESC rd = desc;
		rd.ArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc = { 1, 0 };
		rd.BindFlags = 0;
		rd.MiscFlags = 0;
		if (FAILED(session->device->CreateTexture2D(&rd, nullptr, &resolved))) {
			ctx->Release();
			return false;
		}
		ctx->ResolveSubresource(resolved, 0, tex, srcSub, desc.Format);
		src = resolved;
		srcSub = 0;
	}

	// One texel per point, side by side in a staging texture
	D3D11_TEXTURE2D_DESC sd{};
	sd.Width = (UINT)points.size();
	sd.Height = 1;
	sd.MipLevels = 1;
	sd.ArraySize = 1;
	sd.Format = desc.Format;
	sd.SampleDesc = { 1, 0 };
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D* staging = nullptr;
	bool ok = SUCCEEDED(session->device->CreateTexture2D(&sd, nullptr, &staging));
	if (ok) {
		for (size_t i = 0; i < points.size(); i++) {
			UINT x = (UINT)std::min<int32_t>(std::max<int32_t>(points[i].x, 0), (int32_t)desc.Width - 1);
			UINT y = (UINT)std::min<int32_t>(std::max<int32_t>(points[i].y, 0), (int32_t)desc.Height - 1);
			D3D11_BOX box{ x, y, 0, x + 1, y + 1, 1 };
			ctx->CopySubresourceRegion(staging, 0, (UINT)i, 0, 0, src, srcSub, &box);
		}
		D3D11_MAPPED_SUBRESOURCE mapped;
		ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
		if (ok) {
			out.assign((const uint8_t*)mapped.pData, (const uint8_t*)mapped.pData + points.size() * bytesPerTexel);
			ctx->Unmap(staging, 0);
		}
		staging->Release();
	}
	if (resolved)
		resolved->Release();
	ctx->Release();
	return ok;
}

} // namespace

const RtGfx& RtD3D11Gfx()
{
	static const RtGfx gfx = {
		RtApi::D3D11, "d3d11", "XR_KHR_D3D11_enable", nullptr, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR, XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR,
		Bind, Unbind, Formats, DefaultFormats, Create, Destroy, Enumerate, LayoutOfDxgi, false, Read,
	};
	return gfx;
}

PFN_xrVoidFunction RtD3D11Lookup(const char* name)
{
	if (!std::strcmp(name, "xrGetD3D11GraphicsRequirementsKHR"))
		return (PFN_xrVoidFunction)GetD3D11GraphicsRequirements;
	return nullptr;
}
