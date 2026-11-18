// The helper programs' cases: overlay and background applications, alone and beside a scene app (run.py --jobs 2 with
// a shared store). Each writes one record per step, as cases.cpp's do.

#include "conformance.h"

#include "IVRCompositor_029.h"
#include "IVROverlay_028.h"
#include "IVRSystem_022.h"

#include "gfx/GfxApi.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

using namespace conformance;

namespace {

vr::HmdMatrix34_t At(float x, float y, float z)
{
	vr::HmdMatrix34_t m{};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
	m.m[0][3] = x;
	m.m[1][3] = y;
	m.m[2][3] = z;
	return m;
}

template <typename T>
T* Interface(CaseContext& c, const char* name)
{
	vr::EVRInitError e = vr::VRInitError_None;
	return (T*)c.core->GetGenericInterface(name, &e);
}

// The dump's session, its frames and the types of the layers it kept
Json::Value DumpSummary()
{
	Json::Value r;
	Json::Value dump = ReadStandInDump();
	if (dump.isNull())
		return r;
	r["binding"] = dump["session"]["binding"];
	r["overlay"] = dump["session"]["overlay"];
	r["frame_order_errors"] = dump["frame_order_errors"];
	r["frames_ended"] = dump["frames_ended"];
	std::set<std::string> types;
	for (const Json::Value& layer : dump["layers"])
		types.insert(layer["type"].asString());
	r["layer_types"] = Json::Value(Json::arrayValue);
	for (const std::string& t : types)
		r["layer_types"].append(t);
	return r;
}

// An overlay application with one overlay from memory and nothing else: no WaitGetPoses, so every frame is
// OpenLamina's own. About 60 frames' time at 90 Hz.
void OverlayAppCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record("frames", r);
		return;
	}
	auto* ov = Interface<IVROverlay>(c, "IVROverlay_028");
	r["overlay_interface"] = ov != nullptr;
	if (ov) {
		vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
		r["create"] = (int)ov->CreateOverlay("oc.helper.raw", "Helper", &h);
		std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
		r["raw"] = (int)ov->SetOverlayRaw(h, grey.data(), 64, 64, 4);
		vr::HmdMatrix34_t m = At(0, 1.5f, -2);
		ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
		r["show"] = (int)ov->ShowOverlay(h);
		std::this_thread::sleep_for(std::chrono::milliseconds(667));
		ov->DestroyOverlay(h);
	}
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["session_overlay"] = dump["overlay"];
	r["layer_types"] = dump["layer_types"];
	r["frame_order_errors"] = dump["frame_order_errors"];
	r["frames_at_least_40"] = dump["frames_ended"].asInt() >= 40;
	c.Record("frames", r);
}

// An overlay application's textures of each API, at once: each is copied onto OpenLamina's own D3D11 device (the
// overlay session's), and its quad shows the test pattern. A kind whose device can't be made here skips the case.
struct OverlayTextureKind {
	GfxKind kind;
	int64_t format;
	const char* name;
};

void OverlayAppTexturesCase(CaseContext& c, const char* label, std::vector<OverlayTextureKind> kinds)
{
	using namespace vr::IVROverlay_028;
	std::vector<std::unique_ptr<GfxApi>> apis;
	std::vector<GfxTexture> textures;
	for (const OverlayTextureKind& k : kinds) {
		std::string why;
		apis.push_back(GfxApi::Create(k.kind, &why));
		if (!apis.back()) {
			Json::Value r;
			r["skipped"] = why;
			c.Record(label, r);
			return;
		}
		textures.push_back(apis.back()->Pattern(64, 64, k.format, 0));
		apis.back()->Flush();
	}
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Overlay, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record(label, r);
		return;
	}
	auto* ov = Interface<IVROverlay>(c, "IVROverlay_028");
	if (ov) {
		std::vector<vr::VROverlayHandle_t> handles(kinds.size(), vr::k_ulOverlayHandleInvalid);
		for (size_t i = 0; i < kinds.size(); i++) {
			ov->CreateOverlay((std::string("oc.helper.") + kinds[i].name).c_str(), kinds[i].name, &handles[i]);
			vr::HmdMatrix34_t m = At((float)i * 1.2f, 1.5f, -2);
			ov->SetOverlayTransformAbsolute(handles[i], vr::TrackingUniverseStanding, &m);
			r[kinds[i].name] = (int)ov->SetOverlayTexture(handles[i], textures[i].Texture());
			ov->ShowOverlay(handles[i]);
		}
		// Again, as programs set their textures every frame
		r["again"] = (int)ov->SetOverlayTexture(handles[0], textures[0].Texture());
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		for (vr::VROverlayHandle_t h : handles)
			ov->DestroyOverlay(h);
	}
	c.core->Cleanup();
	Json::Value summary = DumpSummary();
	r["binding"] = summary["binding"];
	r["session_overlay"] = summary["overlay"];
	r["frame_order_errors"] = summary["frame_order_errors"];
	// The last frame's quads that show the pattern: red top left, white bottom right
	Json::Value dump = ReadStandInDump();
	const Json::Value& layers = dump["layers"];
	const Json::Value lastFrame = layers.size() ? layers[layers.size() - 1]["frame"] : Json::Value();
	int shown = 0;
	for (const Json::Value& layer : layers) {
		if (layer["frame"] == lastFrame && layer["type"].asString() == "quad" && layer["samples"]["tl"].asString() == "ff0000ff"
		    && layer["samples"]["br"].asString() == "ffffffff")
			shown++;
	}
	r["quads_with_pattern"] = shown;
	c.Record(label, r);
	for (auto& api : apis)
		api->Flush();
}

// A background application: a headless session, whose poses and events work, and which can't submit
void BackgroundAppCase(CaseContext& c)
{
	Json::Value r;
	r["init"] = (int)c.core->Init(vr::VRApplication_Background, nullptr);
	if (r["init"].asInt() != vr::VRInitError_None) {
		c.Record("session", r);
		return;
	}
	auto* sys = Interface<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	auto* comp = Interface<vr::IVRCompositor_029::IVRCompositor>(c, "IVRCompositor_029");
	r["system"] = sys != nullptr;
	r["compositor"] = comp != nullptr;
	bool valid = false;
	for (int i = 0; i < 50 && sys && !valid; i++) {
		vr::VREvent_t ev{};
		while (sys->PollNextEvent(&ev, sizeof(ev))) {
		}
		vr::TrackedDevicePose_t pose{};
		sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &pose, 1);
		valid = pose.bPoseIsValid;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	r["hmd_pose_valid"] = valid;
	if (sys)
		r["hmd_connected"] = sys->IsTrackedDeviceConnected(0);
	if (comp) {
		vr::TrackedDevicePose_t poses[4];
		r["wait_get_poses"] = (int)comp->WaitGetPoses(poses, 4, nullptr, 0);
		callers::Texture tex(ArgSet::Valid, *c.call);
		r["submit"] = (int)comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
	}
	c.core->Cleanup();
	Json::Value dump = DumpSummary();
	r["binding"] = dump["binding"];
	r["session_overlay"] = dump["overlay"];
	// No frame calls on a headless session (Monado answers them CALL_ORDER_INVALID), and its poses located at the time
	// now, not at its session's start
	Json::Value raw = ReadStandInDump();
	r["frame_calls"] = raw["frames_waited"].asInt() + raw["frames_begun"].asInt() + raw["frames_ended"].asInt();
	r["frame_order_errors"] = raw["frame_order_errors"].asInt();
	r["stale_locates"] = raw["stale_locates"].asInt();
	c.Record("session", r);
}

// Without the runtime's overlay and headless extensions (or with a runtime that fails to start): the refusals,
// 130 InvalidApplicationType and 121 NoServerForBackgroundApp
void HelperAppsUnsupportedCase(CaseContext& c)
{
	Json::Value r;
	for (vr::EVRApplicationType t : { vr::VRApplication_Overlay, vr::VRApplication_Background }) {
		vr::EVRInitError e = c.core->Init(t, nullptr);
		r[t == vr::VRApplication_Overlay ? "overlay" : "background"] = (int)e;
		if (e == vr::VRInitError_None)
			c.core->Cleanup();
	}
	c.Record("init", r);
}

} // namespace

const std::map<std::string, CaseFn>& conformance::HelperCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "overlay-app", OverlayAppCase },
		{ "overlay-app-two-apis", [](CaseContext& c) {
			 OverlayAppTexturesCase(c, "textures", { { GfxKind::D3D11, 29, "d3d11" }, { GfxKind::D3D12, 29, "d3d12" } });
		 } },
		{ "overlay-app-vk", [](CaseContext& c) { OverlayAppTexturesCase(c, "textures", { { GfxKind::Vulkan, 43, "vulkan" } }); } },
		{ "overlay-app-gl", [](CaseContext& c) { OverlayAppTexturesCase(c, "textures", { { GfxKind::OpenGL, 0x8C43, "gl" } }); } },
		{ "background-app", BackgroundAppCase },
		{ "helper-apps-unsupported", HelperAppsUnsupportedCase },
	};
	return cases;
}
