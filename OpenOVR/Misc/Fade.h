#pragma once
// The compositor's fades over time (the scene fade, the background fade, the grid), and the skybox a program asked for:
// what each is at a moment, and which layers a frame shows. Times are seconds on any steady clock.

#include "generated/interfaces/vrtypes.h"

#include <cstdint>
#include <mutex>
#include <vector>

// Seconds on the clock the fades are read by
double CompositorClockSeconds();

// A value that moves linearly to a target over a time, from where it is when told
template <typename T>
class Ramp {
public:
	explicit Ramp(T initial = T())
	    : from(initial)
	    , to(initial)
	{
	}
	// seconds <= 0 (or NaN) is at once
	void Start(double now, double seconds, T target);
	T At(double now) const;

private:
	T from, to;
	double start = 0, length = 0;
};

class CompositorFades {
public:
	static CompositorFades& Get();

	void FadeToColour(double now, double seconds, vr::HmdColor_t target, bool background);
	// Un-premultiplied, as it was asked for
	vr::HmdColor_t ColourAt(double now, bool background) const;
	void FadeGrid(double now, double seconds, bool in);
	float GridAlphaAt(double now) const;
	// Whether the grid was last asked to fade in (reached or not)
	bool GridWanted() const;
	// Everything back to no fade and no grid
	void Reset();

private:
	mutable std::mutex lock;
	Ramp<vr::HmdColor_t> scene{ vr::HmdColor_t{ 0, 0, 0, 0 } };
	Ramp<vr::HmdColor_t> background{ vr::HmdColor_t{ 0, 0, 0, 0 } };
	Ramp<float> grid{ 0.0f };
	bool gridWanted = false;
};

// What a frame shows of the scene and the skybox
struct ScenePlan {
	bool scene; // the game's layer
	bool skybox; // the skybox's layers, in its place
};
// The skybox replaces the game's layer once the grid is fully faded in, if there is one to show
ScenePlan PlanScene(bool haveScene, bool haveSkybox, float gridAlpha);
// Whether a fade colour changes what's seen
bool FadeVisible(const vr::HmdColor_t& colour);
// The colour as 8-bit texels, un-premultiplied, rounded and clamped
void FadeTexel(const vr::HmdColor_t& colour, uint8_t out[4]);

// The skybox's textures as asked for, kept to be shown later. The Texture_t values are copied, with what a Vulkan or
// D3D12 one's handle points to; the textures themselves stay the program's.
class SkyboxRequest {
public:
	// False if the textures can't be a skybox (SkyboxTexturesValid)
	bool Set(const vr::Texture_t* textures, uint32_t count);
	void Clear();
	bool Empty() const { return textures.empty(); }
	const vr::Texture_t* Textures() const { return textures.data(); }
	uint32_t Count() const { return (uint32_t)textures.size(); }

	// Whether the textures stay alive while it's kept, whatever the game does with them: Direct3D 11 and 12 ones, of
	// which it holds a COM reference (Vulkan and OpenGL ones are the game's to keep)
	bool HeldAlive() const;

	SkyboxRequest() = default;
	SkyboxRequest(const SkyboxRequest& other) { Copy(other); }
	SkyboxRequest& operator=(const SkyboxRequest& other)
	{
		if (this != &other)
			Copy(other);
		return *this;
	}
	~SkyboxRequest() { Hold(false); }

private:
	void Copy(const SkyboxRequest& other);
	void Fix();
	// Takes (true) or drops a reference of each Direct3D texture's (and D3D12 queue's)
	void Hold(bool take);
	std::vector<vr::Texture_t> textures;
	std::vector<vr::VRVulkanTextureData_t> vulkan; // one for each texture, used by the Vulkan ones
	std::vector<vr::D3D12TextureData_t> d3d12; // the same for the D3D12 ones
};

// One texture (lat-long), two (a stereo pair) or six (cube faces), each with a handle
bool SkyboxTexturesValid(const vr::Texture_t* textures, uint32_t count);
