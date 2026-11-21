#include "Fade.h"

#include <algorithm>
#ifdef _WIN32
#include <unknwn.h>
#endif
#include <chrono>
#include <cmath>

namespace {
float Lerp(float a, float b, float t) { return a + (b - a) * t; }

vr::HmdColor_t Lerp(const vr::HmdColor_t& a, const vr::HmdColor_t& b, float t)
{
	return { Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t) };
}
} // namespace

double CompositorClockSeconds()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

template <typename T>
void Ramp<T>::Start(double now, double seconds, T target)
{
	from = At(now);
	to = target;
	start = now;
	length = seconds > 0 ? seconds : 0; // NaN is no time
}

template <typename T>
T Ramp<T>::At(double now) const
{
	if (!(length > 0))
		return to;
	if (!(now > start))
		return from;
	const double t = (now - start) / length;
	return t >= 1 ? to : Lerp(from, to, (float)t);
}

template class Ramp<float>;
template class Ramp<vr::HmdColor_t>;

CompositorFades& CompositorFades::Get()
{
	static CompositorFades fades;
	return fades;
}

void CompositorFades::FadeToColour(double now, double seconds, vr::HmdColor_t target, bool isBackground)
{
	std::lock_guard<std::mutex> guard(lock);
	(isBackground ? background : scene).Start(now, seconds, target);
}

vr::HmdColor_t CompositorFades::ColourAt(double now, bool isBackground) const
{
	std::lock_guard<std::mutex> guard(lock);
	return (isBackground ? background : scene).At(now);
}

void CompositorFades::FadeGrid(double now, double seconds, bool in)
{
	std::lock_guard<std::mutex> guard(lock);
	grid.Start(now, seconds, in ? 1.0f : 0.0f);
	gridWanted = in;
}

float CompositorFades::GridAlphaAt(double now) const
{
	std::lock_guard<std::mutex> guard(lock);
	return grid.At(now);
}

bool CompositorFades::GridWanted() const
{
	std::lock_guard<std::mutex> guard(lock);
	return gridWanted;
}

void CompositorFades::Reset()
{
	std::lock_guard<std::mutex> guard(lock);
	scene = Ramp<vr::HmdColor_t>(vr::HmdColor_t{ 0, 0, 0, 0 });
	background = scene;
	grid = Ramp<float>(0.0f);
	gridWanted = false;
}

ScenePlan PlanScene(bool haveScene, bool haveSkybox, float gridAlpha)
{
	const bool inSkybox = haveSkybox && gridAlpha >= 1.0f;
	return { haveScene && !inSkybox, inSkybox };
}

bool FadeVisible(const vr::HmdColor_t& colour)
{
	return colour.a > 0.0f;
}

void FadeTexel(const vr::HmdColor_t& colour, uint8_t out[4])
{
	const float values[4] = { colour.r, colour.g, colour.b, colour.a };
	for (int i = 0; i < 4; i++) {
		const float v = std::isnan(values[i]) ? 0.0f : std::clamp(values[i], 0.0f, 1.0f);
		out[i] = (uint8_t)std::lround(v * 255.0f);
	}
}

bool SkyboxTexturesValid(const vr::Texture_t* textures, uint32_t count)
{
	if (!textures || (count != 1 && count != 2 && count != 6))
		return false;
	for (uint32_t i = 0; i < count; i++)
		if (!textures[i].handle)
			return false;
	return true;
}

bool SkyboxRequest::Set(const vr::Texture_t* in, uint32_t count)
{
	if (!SkyboxTexturesValid(in, count))
		return false;
	Hold(false);
	textures.assign(in, in + count);
	vulkan.assign(count, vr::VRVulkanTextureData_t{});
	d3d12.assign(count, vr::D3D12TextureData_t{});
	for (uint32_t i = 0; i < count; i++) {
		if (in[i].eType == vr::TextureType_Vulkan)
			vulkan[i] = *(const vr::VRVulkanTextureData_t*)in[i].handle;
		else if (in[i].eType == vr::TextureType_DirectX12)
			d3d12[i] = *(const vr::D3D12TextureData_t*)in[i].handle;
	}
	Fix();
	Hold(true);
	return true;
}

void SkyboxRequest::Clear()
{
	Hold(false);
	textures.clear();
	vulkan.clear();
	d3d12.clear();
}

void SkyboxRequest::Copy(const SkyboxRequest& other)
{
	Hold(false);
	textures = other.textures;
	vulkan = other.vulkan;
	d3d12 = other.d3d12;
	Fix();
	Hold(true);
}

bool SkyboxRequest::HeldAlive() const
{
#ifdef _WIN32
	return std::all_of(textures.begin(), textures.end(),
	    [](const vr::Texture_t& t) { return t.eType == vr::TextureType_DirectX || t.eType == vr::TextureType_DirectX12; });
#else
	return textures.empty();
#endif
}

void SkyboxRequest::Hold(bool take)
{
#ifdef _WIN32
	auto hold = [take](void* object) {
		if (!object)
			return;
		// Every COM interface starts with IUnknown's
		auto* unknown = reinterpret_cast<IUnknown*>(object);
		if (take)
			unknown->AddRef();
		else
			unknown->Release();
	};
	for (size_t i = 0; i < textures.size(); i++) {
		if (textures[i].eType == vr::TextureType_DirectX) {
			hold(textures[i].handle);
		} else if (textures[i].eType == vr::TextureType_DirectX12 && i < d3d12.size()) {
			hold(d3d12[i].m_pResource);
			hold(d3d12[i].m_pCommandQueue);
		}
	}
#else
	(void)take;
#endif
}

// The copied Vulkan and D3D12 textures' handles point to the copies of what they pointed to (the game's structs are
// often on its stack). Direct3D textures are kept alive by Hold's references; Vulkan and OpenGL images are the game's.
void SkyboxRequest::Fix()
{
	for (size_t i = 0; i < textures.size(); i++) {
		if (textures[i].eType == vr::TextureType_Vulkan)
			textures[i].handle = &vulkan[i];
		else if (textures[i].eType == vr::TextureType_DirectX12)
			textures[i].handle = &d3d12[i];
	}
}
