// The named cases of oc_conformance: sequences the per-method callers can't do (real handles, frames, several Init
// rounds). Each writes one record per step: {"case", "method": <step label>, ...}.

#include "conformance.h"

#include "IVRApplications_001.h"
#include "IVRApplications_007.h"
#include "IVRChaperone_002.h"
#include "IVRChaperone_004.h"
#include "IVRChaperoneSetup_006.h"
#include "IVRCompositor_005.h"
#include "IVRControlPanel_001.h"
#include "IVRDriverManager_001.h"
#include "custom_interfaces/IVRPaths_002.h"
#include "IVRSpatialAnchors_001.h"
#include "IVRTrackedCamera_001.h"
#include "IVRTrackedCamera_006.h"
#include "IVRInput_003.h"
#include "IVRNotifications_001.h"
#include "IVRRenderModels_001.h"
#include "IVRRenderModels_006.h"
#include "IVRCompositor_006.h"
#include "IVRCompositor_008.h"
#include "IVRCompositor_029.h"
#include "IVRExtendedDisplay_001.h"
#include "IVRInput_010.h"
#include "IVRInput_011.h"
#include "IVROverlay_001.h"
#include "IVROverlay_006.h"
#include "IVROverlay_007.h"
#include "IVROverlay_025.h"
#include "IVROverlay_028.h"
#include "IVRSystem_003.h"
#include "IVRSystem_007.h"
#include "IVRSystem_009.h"
#include "IVRSystem_022.h"
#include "IVRSystem_026.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <atomic>
#include <set>
#include <map>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "gfx/GfxApi.h"

#ifdef _WIN32
#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

using namespace conformance;

// The stand-in runtime's dump (OC_TEST_RUNTIME_DUMP), written when OpenLamina destroys its instance: read it after
// Cleanup. Null without one (another runtime, or none written).
Json::Value conformance::ReadStandInDump()
{
	const char* path = std::getenv("OC_TEST_RUNTIME_DUMP");
	if (!path || !*path)
		return Json::Value();
	std::ifstream in(path, std::ios::binary);
	Json::Value root;
	Json::CharReaderBuilder rb;
	std::string errors;
	if (!in || !Json::parseFromStream(rb, in, &root, &errors))
		return Json::Value();
	return root;
}

namespace {

Json::Value Err(int e)
{
	Json::Value v;
	v["ret"] = e;
	return v;
}

void InitMatrix(CaseContext& c)
{
	std::vector<int> types;
	for (int t = 0; t <= 13; t++)
		types.push_back(t);
	types.push_back(99);
	for (int t : types) {
		vr::EVRInitError e = c.core->Init((vr::EVRApplicationType)t, nullptr);
		Json::Value r;
		r["init_error"] = (int)e;
		c.Record("Init(" + std::to_string(t) + ")", r);
		if (e == vr::VRInitError_None)
			c.core->Cleanup();
	}
	Json::Value r;
	r["ret"] = c.core->BIsHmdPresent();
	c.Record("BIsHmdPresent", r);
}

void InitRepeat(CaseContext& c)
{
	int ok = 0, found = 0;
	vr::EVRInitError last = vr::VRInitError_None;
	for (int i = 0; i < 200; i++) {
		last = c.core->Init(vr::VRApplication_Scene, nullptr);
		if (last != vr::VRInitError_None)
			continue;
		ok++;
		vr::EVRInitError le;
		if (c.core->GetGenericInterface("IVRSystem_022", &le))
			found++;
		c.core->Cleanup();
	}
	Json::Value r;
	r["inits_ok"] = ok;
	r["interfaces_found"] = found;
	r["last_init_error"] = (int)last;
	c.Record("200 rounds", r);
}

void InitOtherThread(CaseContext& c)
{
	vr::EVRInitError e = vr::VRInitError_None;
	std::thread t([&] { e = c.core->Init(vr::VRApplication_Scene, nullptr); });
	t.join();
	Json::Value r;
	r["init_error"] = (int)e;
	c.Record("Init on another thread", r);
	if (e != vr::VRInitError_None)
		return;
	vr::EVRInitError le;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	Json::Value r2;
	r2["lookup_error"] = (int)le;
	if (sys) {
		uint32_t w = 0, h = 0;
		sys->GetRecommendedRenderTargetSize(&w, &h);
		r2["width"] = w;
		r2["height"] = h;
	}
	c.Record("GetRecommendedRenderTargetSize on the main thread", r2);
	c.core->Cleanup();
}

void LookupBeforeInit(CaseContext& c)
{
	auto lookups = [&](const char* when) {
		for (const std::string& v : GeneratedVersions()) {
			vr::EVRInitError le = vr::VRInitError_None;
			void* p = c.core->GetGenericInterface(v.c_str(), &le);
			Json::Value r;
			r["lookup_error"] = (int)le;
			r["found"] = p != nullptr;
			c.Record(std::string(when) + " " + v, r);
		}
	};
	lookups("before Init:");
	if (c.core->Init(vr::VRApplication_Scene, nullptr) == vr::VRInitError_None)
		c.core->Cleanup();
	lookups("after Cleanup:");
}

void UnknownInterface(CaseContext& c)
{
	vr::EVRInitError e = c.core->Init(vr::VRApplication_Scene, nullptr);
	for (const char* name : { "IVRNope_001", "IVRSystem_999", "FnTable:IVRSystem_022", "FnTable:IVRNope_001" }) {
		vr::EVRInitError le = vr::VRInitError_None;
		void* p = c.core->GetGenericInterface(name, &le);
		Json::Value r;
		r["init_error"] = (int)e;
		r["lookup_error"] = (int)le;
		r["found"] = p != nullptr;
		c.Record(name, r);
	}
	if (e == vr::VRInitError_None)
		c.core->Cleanup();
}

void OverlayLifecycle(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* ov = (IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!ov) {
		c.Record("no IVROverlay_028", Err(le));
		c.core->Cleanup();
		return;
	}
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	c.Record("CreateOverlay", Err(ov->CreateOverlay("oc.test", "Test", &h)));

	using SetFloat = vr::EVROverlayError (IVROverlay::*)(vr::VROverlayHandle_t, float);
	using GetFloat = vr::EVROverlayError (IVROverlay::*)(vr::VROverlayHandle_t, float*);
	auto setGetFloat = [&](const char* name, SetFloat setter, GetFloat getter, float value) {
		Json::Value r;
		r["set"] = (int)(ov->*setter)(h, value);
		float got = -1;
		r["get"] = (int)(ov->*getter)(h, &got);
		r["value"] = got;
		r["expected"] = value;
		c.Record(name, r);
	};
	setGetFloat("Alpha", &IVROverlay::SetOverlayAlpha, &IVROverlay::GetOverlayAlpha, 0.5f);
	setGetFloat("WidthInMeters", &IVROverlay::SetOverlayWidthInMeters, &IVROverlay::GetOverlayWidthInMeters, 1.5f);
	setGetFloat("Curvature", &IVROverlay::SetOverlayCurvature, &IVROverlay::GetOverlayCurvature, 0.25f);
	setGetFloat("PreCurvePitch", &IVROverlay::SetOverlayPreCurvePitch, &IVROverlay::GetOverlayPreCurvePitch, 0.125f);
	setGetFloat("TexelAspect", &IVROverlay::SetOverlayTexelAspect, &IVROverlay::GetOverlayTexelAspect, 2.0f);
	{
		Json::Value r;
		r["set"] = (int)ov->SetOverlaySortOrder(h, 3);
		uint32_t got = 0;
		r["get"] = (int)ov->GetOverlaySortOrder(h, &got);
		r["value"] = got;
		r["expected"] = 3;
		c.Record("SortOrder", r);
	}
	{
		Json::Value r;
		r["set"] = (int)ov->SetOverlayRenderingPid(h, 1234);
		r["value"] = ov->GetOverlayRenderingPid(h);
		r["expected"] = 1234;
		c.Record("RenderingPid", r);
	}
	{
		Json::Value r;
		r["set"] = (int)ov->SetOverlayColor(h, 0.25f, 0.5f, 0.75f);
		float cr = 0, cg = 0, cb = 0;
		r["get"] = (int)ov->GetOverlayColor(h, &cr, &cg, &cb);
		r["value"] = cr + cg * 10 + cb * 100;
		r["expected"] = 0.25 + 5.0 + 75.0;
		c.Record("Color", r);
	}
	{
		Json::Value r;
		r["set"] = (int)ov->SetOverlayFlag(h, VROverlayFlags_SideBySide_Parallel, true);
		bool got = false;
		r["get"] = (int)ov->GetOverlayFlag(h, VROverlayFlags_SideBySide_Parallel, &got);
		r["value"] = got;
		r["expected"] = true;
		c.Record("Flag", r);
	}
	{
		vr::HmdMatrix34_t m{};
		m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
		m.m[1][3] = 1.25f;
		Json::Value r;
		r["set"] = (int)ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
		vr::HmdMatrix34_t got{};
		vr::ETrackingUniverseOrigin origin = vr::TrackingUniverseSeated;
		r["get"] = (int)ov->GetOverlayTransformAbsolute(h, &origin, &got);
		r["value"] = got.m[1][3];
		r["expected"] = 1.25f;
		c.Record("TransformAbsolute", r);
	}
	vr::Texture_t tex{};
	callers::Texture made(ArgSet::Valid, *c.call);
	c.Record("SetOverlayTexture", Err(made.get() ? (int)ov->SetOverlayTexture(h, made.get()) : -1));
	c.Record("ShowOverlay", Err(ov->ShowOverlay(h)));
	{
		Json::Value r;
		r["ret"] = ov->IsOverlayVisible(h);
		c.Record("IsOverlayVisible", r);
	}
	c.Record("HideOverlay", Err(ov->HideOverlay(h)));
	c.Record("DestroyOverlay", Err(ov->DestroyOverlay(h)));
	(void)tex;
	c.core->Cleanup();
}

void InputManifest(CaseContext& c)
{
	using namespace vr::IVRInput_010;
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* in = (IVRInput*)c.core->GetGenericInterface("IVRInput_010", &le);
	if (!in) {
		c.Record("no IVRInput_010", Err(le));
		c.core->Cleanup();
		return;
	}
	c.Record("SetActionManifestPath", Err(in->SetActionManifestPath(c.manifest.c_str())));
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	c.Record("GetActionSetHandle", Err(in->GetActionSetHandle("/actions/main", &set)));
	vr::VRActionHandle_t trigger = 0, grab = 0, pose = 0, haptic = 0;
	c.Record("GetActionHandle trigger", Err(in->GetActionHandle("/actions/main/in/trigger", &trigger)));
	c.Record("GetActionHandle grab", Err(in->GetActionHandle("/actions/main/in/grab", &grab)));
	c.Record("GetActionHandle pose", Err(in->GetActionHandle("/actions/main/in/pose", &pose)));
	c.Record("GetActionHandle haptic", Err(in->GetActionHandle("/actions/main/out/haptic", &haptic)));
	VRActiveActionSet_t active{};
	active.ulActionSet = set;
	c.Record("UpdateActionState", Err(in->UpdateActionState(&active, sizeof(active), 1)));
	InputDigitalActionData_t digital{};
	c.Record("GetDigitalActionData grab", Err(in->GetDigitalActionData(grab, &digital, sizeof(digital), vr::k_ulInvalidInputValueHandle)));
	InputAnalogActionData_t analog{};
	c.Record("GetAnalogActionData trigger", Err(in->GetAnalogActionData(trigger, &analog, sizeof(analog), vr::k_ulInvalidInputValueHandle)));
	InputPoseActionData_t poseData{};
	c.Record("GetPoseActionDataForNextFrame pose",
	    Err(in->GetPoseActionDataForNextFrame(pose, vr::TrackingUniverseStanding, &poseData, sizeof(poseData), vr::k_ulInvalidInputValueHandle)));
	c.Record("TriggerHapticVibrationAction", Err(in->TriggerHapticVibrationAction(haptic, 0, 0.1f, 160, 0.5f, vr::k_ulInvalidInputValueHandle)));
	c.core->Cleanup();
}

struct Frames {
	vr::IVRCompositor_029::IVRCompositor* comp = nullptr;
	vr::IVRSystem_022::IVRSystem* sys = nullptr;
};

bool OpenFrames(CaseContext& c, Frames& f)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return false;
	}
	vr::EVRInitError le;
	f.comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
	f.sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	if (!f.comp || !f.sys) {
		c.Record("no compositor or system", Err(le));
		c.core->Cleanup();
		return false;
	}
	return true;
}

// Rounded to a millimetre (or a thousandth), so a record compares exactly
double Milli(double v)
{
	double r = std::round(v * 1000) / 1000;
	return r == 0 ? 0.0 : r; // no -0
}

Json::Value MilliArray(const Json::Value& a)
{
	Json::Value out(Json::arrayValue);
	for (const Json::Value& v : a)
		out.append(Milli(v.asDouble()));
	return out;
}

Json::Value MilliArray(std::initializer_list<float> values)
{
	Json::Value out(Json::arrayValue);
	for (float v : values)
		out.append(Milli(v));
	return out;
}

vr::HmdMatrix34_t Translation(float x, float y, float z)
{
	vr::HmdMatrix34_t m{};
	m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
	m.m[0][3] = x;
	m.m[1][3] = y;
	m.m[2][3] = z;
	return m;
}

// One overlay per kind of transform, drawn for a few frames: the stand-in's dump has each layer's placement
// (expect/overlay_transforms.json, with scripts/overlay_transforms.json). The head is at (0, 1.7, 0), looking along
// -Z; every overlay is 1 m wide, with a square texture.
void OverlayTransformsCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	auto* ov025 = (vr::IVROverlay_025::IVROverlay*)c.core->GetGenericInterface("IVROverlay_025", &le);
	if (!ov || !ov025) {
		c.Record("no IVROverlay_028 or 025", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	std::vector<std::pair<std::string, vr::VROverlayHandle_t>> made;
	auto make = [&](const char* name) {
		vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
		ov->CreateOverlay((std::string("oc.transforms.") + name).c_str(), name, &h);
		ov->SetOverlayWidthInMeters(h, 1);
		ov->SetOverlaySortOrder(h, (uint32_t)made.size() + 1); // drawn in this order
		if (tex.get())
			ov->SetOverlayTexture(h, tex.get());
		ov->ShowOverlay(h);
		made.emplace_back(name, h);
		return h;
	};
	Json::Value set;
	vr::HmdMatrix34_t m = Translation(0, 1.5f, -2);
	vr::VROverlayHandle_t absolute = make("absolute");
	set["absolute"] = (int)ov->SetOverlayTransformAbsolute(absolute, vr::TrackingUniverseStanding, &m);
	m = Translation(0.5f, 1, -2);
	set["seated"] = (int)ov->SetOverlayTransformAbsolute(make("seated"), vr::TrackingUniverseSeated, &m);
	m = Translation(0, 0, -1.5f);
	set["device"] = (int)ov->SetOverlayTransformTrackedDeviceRelative(make("device"), vr::k_unTrackedDeviceIndex_Hmd, &m);
	set["component"] = (int)ov->SetOverlayTransformTrackedDeviceComponent(make("component"), vr::k_unTrackedDeviceIndex_Hmd, "tip");
	// Half a metre right of the absolute overlay, turned 90 degrees to its left
	m = Translation(0.5f, 0, 0);
	m.m[0][0] = m.m[2][2] = 0;
	m.m[0][2] = 1;
	m.m[2][0] = -1;
	set["relative"] = (int)ov025->SetOverlayTransformOverlayRelative(make("relative"), absolute, &m);
	// A cursor nothing points at: at its own transform
	vr::VROverlayHandle_t cursor = make("cursor");
	m = Translation(-0.5f, 1.5f, -2);
	ov->SetOverlayTransformAbsolute(cursor, vr::TrackingUniverseStanding, &m);
	vr::HmdVector2_t hotspot{ { 0.5f, 0.5f } };
	set["cursor"] = (int)ov->SetOverlayTransformCursor(cursor, &hotspot);
	// Projections: 90 degrees for the left eye; 1 left, 0.5 right, 1 up and 0.5 down for the right one
	m = Translation(-0.032f, 1.7f, 0);
	vr::IVROverlay_028::VROverlayProjection_t left{ -1, 1, -1, 1 };
	set["projection left"] = (int)ov->SetOverlayTransformProjection(make("projection left"), vr::TrackingUniverseStanding, &m, &left, vr::Eye_Left);
	m = Translation(0.032f, 1.7f, 0);
	vr::IVROverlay_028::VROverlayProjection_t right{ -1, 0.5f, -1, 0.5f };
	set["projection right"] = (int)ov->SetOverlayTransformProjection(make("projection right"), vr::TrackingUniverseStanding, &m, &right, vr::Eye_Right);
	vr::VROverlayHandle_t curved = make("curved");
	m = Translation(0, 1, -1);
	ov->SetOverlayTransformAbsolute(curved, vr::TrackingUniverseStanding, &m);
	set["curved"] = (int)ov->SetOverlayCurvature(curved, 0.25f);
	// Relative to each other: neither can be placed
	vr::VROverlayHandle_t cycleA = make("cycle a"), cycleB = make("cycle b");
	m = Translation(0, 0, 0);
	set["cycle a"] = (int)ov025->SetOverlayTransformOverlayRelative(cycleA, cycleB, &m);
	set["cycle b"] = (int)ov025->SetOverlayTransformOverlayRelative(cycleB, cycleA, &m);
	c.Record("set", set);

	Json::Value types;
	for (auto& [name, h] : made) {
		vr::IVROverlay_028::VROverlayTransformType type = vr::IVROverlay_028::VROverlayTransform_Invalid;
		ov->GetOverlayTransformType(h, &type);
		types[name] = (int)type;
	}
	c.Record("GetOverlayTransformType", types);

	for (int frame = 1; frame <= 5; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}

	auto intersect = [&](const char* label, vr::VROverlayHandle_t h, vr::HmdVector3_t from, vr::HmdVector3_t dir) {
		vr::IVROverlay_028::VROverlayIntersectionParams_t params{ from, dir, vr::TrackingUniverseStanding };
		vr::IVROverlay_028::VROverlayIntersectionResults_t res{};
		Json::Value r;
		r["ret"] = ov->ComputeOverlayIntersection(h, &params, &res);
		r["point"] = MilliArray({ res.vPoint.v[0], res.vPoint.v[1], res.vPoint.v[2] });
		r["normal"] = MilliArray({ res.vNormal.v[0], res.vNormal.v[1], res.vNormal.v[2] });
		r["uv"] = MilliArray({ res.vUVs.v[0], res.vUVs.v[1] });
		r["distance"] = Milli(res.fDistance);
		c.Record(label, r);
	};
	// From the absolute overlay's height, a quarter of its width in from its right edge
	intersect("ComputeOverlayIntersection absolute", absolute, { { 0, 1.5f, 0 } }, { { 0.25f, 0, -2 } });
	// The curved overlay (a quarter circle) from its cylinder's axis, 0.2 π to the right: u = 0.5 + 0.2 / 0.5
	{
		const float pi = 3.14159265f;
		const float radius = 1 / (2 * pi * 0.25f);
		const float angle = 0.2f * pi;
		intersect("ComputeOverlayIntersection curved", curved, { { 0, 1, -1 + radius } }, { { std::sin(angle), 0, -std::cos(angle) } });
	}
	// A circle in the middle of the absolute overlay: the ray a quarter in misses it, the one at the centre hits it
	{
		vr::IVROverlay_028::VROverlayIntersectionMaskPrimitive_t circle{};
		circle.m_nPrimitiveType = vr::IVROverlay_028::OverlayIntersectionPrimitiveType_Circle;
		circle.m_Primitive.m_Circle = { 0.5f, 0.5f, 0.1f };
		c.Record("SetOverlayIntersectionMask", Err(ov->SetOverlayIntersectionMask(absolute, &circle, 1, sizeof(circle))));
	}
	intersect("masked off centre", absolute, { { 0, 1.5f, 0 } }, { { 0.25f, 0, -2 } });
	intersect("masked centre", absolute, { { 0, 1.5f, 0 } }, { { 0, 0, -1 } });
	{
		vr::HmdMatrix34_t point{};
		Json::Value r;
		r["ret"] = (int)ov->GetTransformForOverlayCoordinates(absolute, vr::TrackingUniverseStanding, { { 0.75f, 0.25f } }, &point);
		r["p"] = MilliArray({ point.m[0][3], point.m[1][3], point.m[2][3] });
		c.Record("GetTransformForOverlayCoordinates", r);
	}
	for (auto& [name, h] : made)
		ov->DestroyOverlay(h);
	c.core->Cleanup();

	// The last frame's overlay layers (after the scene's projection), one record each, in their sort order
	Json::Value dump = ReadStandInDump();
	if (dump.isNull()) {
		c.Record("no dump");
		return;
	}
	const Json::Value& layers = dump["layers"];
	Json::Value last(Json::arrayValue);
	if (layers.size()) {
		const Json::Value frame = layers[layers.size() - 1]["frame"];
		for (const Json::Value& layer : layers)
			if (layer["frame"] == frame && layer["type"].asString() != "projection")
				last.append(layer);
	}
	std::vector<std::string> drawn;
	for (auto& [name, h] : made)
		if (name != "cycle a" && name != "cycle b")
			drawn.push_back(name);
	Json::Value count;
	count["layers"] = last.size();
	count["expected"] = (int)drawn.size();
	c.Record("overlay layers", count);
	for (Json::ArrayIndex i = 0; i < last.size() && i < drawn.size(); i++) {
		const Json::Value& layer = last[i];
		Json::Value r;
		r["type"] = layer["type"];
		r["p"] = MilliArray(layer["pose"]["p"]);
		r["q"] = MilliArray(layer["pose"]["q"]);
		r["eye_visibility"] = layer["eye_visibility"];
		r["invalid"] = layer["invalid"];
		if (layer.isMember("size"))
			r["size"] = MilliArray(layer["size"]);
		if (layer.isMember("radius")) {
			r["radius"] = Milli(layer["radius"].asDouble());
			r["angles"] = MilliArray(layer["angles"]);
			r["aspect_ratio"] = Milli(layer["aspect_ratio"].asDouble());
		}
		c.Record("layer " + drawn[i], r);
	}
}

void FrameLoop(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 120; frame++) {
		vr::TrackedDevicePose_t poses[16];
		Json::Value r;
		r["wait"] = (int)f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		r["left"] = (int)f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		r["right"] = (int)f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		Json::Value events(Json::arrayValue);
		vr::VREvent_t ev{};
		for (int n = 0; n < 64 && f.sys->PollNextEvent(&ev, sizeof(ev)); n++)
			events.append((int)ev.eventType);
		r["events"] = events;
		c.Record("frame " + std::to_string(frame), r);
	}
	c.core->Cleanup();
}

// IVRSystem_003-010 poll into SDK 0.9.x's 32-byte VREvent_t, not today's 64-byte one. 120 frames as frame-loop, polling
// through IVRSystem_009 into a 64-byte buffer: each frame records its events and how many bytes past 32 were written.
// Each kind of event is also recorded once as the old struct reads it ("event <type>"): the device at 4, the data's
// 16 bytes at 8 (hex), eventAgeSeconds at 24 and the 4 bytes after it (Windows' padding). The game reads the right
// controller's old state every frame, so with scripts/legacy_click.json its button events arrive (their data is the
// button's id, expect/old_calls.json)
void OldEvents(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* old = (vr::IVRSystem_009::IVRSystem*)c.core->GetGenericInterface("IVRSystem_009", &le);
	if (!old) {
		c.Record("no IVRSystem_009", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	std::set<uint32_t> laidOut;
	for (int frame = 1; frame <= 120; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		vr::VRControllerState_t state{}; // the same in SDK 0.9.x
		old->GetControllerState(2, &state);
		Json::Value r;
		Json::Value events(Json::arrayValue);
		int overrun = 0;
		for (int n = 0; n < 64; n++) {
			alignas(8) uint8_t buffer[64];
			memset(buffer, 0xCD, sizeof(buffer));
			if (!old->PollNextEvent((vr::VREvent_t*)buffer))
				break;
			uint32_t type;
			memcpy(&type, buffer, sizeof(type));
			events.append((int)type);
			for (int i = 32; i < 64; i++)
				overrun += buffer[i] != 0xCD;
			if (laidOut.insert(type).second) {
				// 0.9.x's layout: eventType 0, trackedDeviceIndex 4, data 8 to 24, eventAgeSeconds 24 (then 4 bytes of
				// padding on Windows, none on Linux, where the struct is 28 bytes)
				uint32_t device, after;
				float age;
				memcpy(&device, buffer + 4, sizeof(device));
				memcpy(&age, buffer + 24, sizeof(age));
				memcpy(&after, buffer + 28, sizeof(after));
				char hex[33];
				for (int i = 0; i < 16; i++)
					std::snprintf(hex + 2 * i, 3, "%02x", buffer[8 + i]);
				Json::Value e;
				e["device"] = device;
				e["data"] = hex;
				e["age"] = age;
				e["age_is_finite"] = std::isfinite(age);
				// Windows' 4 bytes of padding: zeroed; on Linux the struct ends at 28 and they're the buffer's own
				e["after_age"] = after == 0 ? "zero" : after == 0xCDCDCDCD ? "untouched" : "written";
				c.Record("event " + std::to_string(type), e);
			}
		}
		r["events"] = events;
		r["overrun"] = overrun;
		c.Record("frame " + std::to_string(frame), r);
	}
	c.core->Cleanup();
}

Json::Value PoseRecord(const vr::TrackedDevicePose_t& p)
{
	Json::Value r;
	for (int i = 0; i < 3; i++)
		r["position"].append(p.mDeviceToAbsoluteTracking.m[i][3]);
	for (int i = 0; i < 3; i++)
		r["velocity"].append(p.vVelocity.v[i]);
	for (int i = 0; i < 3; i++)
		r["angular_velocity"].append(p.vAngularVelocity.v[i]);
	r["valid"] = p.bPoseIsValid;
	return r;
}

// The old IVRSystem calls whose answers can be checked without frames (expect/old_calls.json)
void OldCalls(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	auto* sys007 = (vr::IVRSystem_007::IVRSystem*)c.core->GetGenericInterface("IVRSystem_007", &le);
	auto* sys003 = (vr::IVRSystem_003::IVRSystem*)c.core->GetGenericInterface("IVRSystem_003", &le);
	if (!sys || !sys007 || !sys003) {
		c.Record("no IVRSystem_022, 007 or 003", Err(le));
		c.core->Cleanup();
		return;
	}

	// A pose at (1, 2, 3) moving along +x and turning about +y, moved by a quarter turn about +y then 10 m along +x
	vr::TrackedDevicePose_t pose = {};
	pose.mDeviceToAbsoluteTracking = { { { 1, 0, 0, 1 }, { 0, 1, 0, 2 }, { 0, 0, 1, 3 } } };
	pose.vVelocity = { { 1, 0, 0 } };
	pose.vAngularVelocity = { { 0, 1, 0 } };
	pose.bPoseIsValid = true;
	vr::HmdMatrix34_t transform = { { { 0, 0, 1, 10 }, { 0, 1, 0, 0 }, { -1, 0, 0, 0 } } };
	vr::TrackedDevicePose_t out = {};
	sys->ApplyTransform(&out, &pose, &transform);
	c.Record("ApplyTransform", PoseRecord(out));
	vr::TrackedDevicePose_t byRef = {};
	sys007->ApplyTransform(&byRef, pose, transform);
	c.Record("ApplyTransform IVRSystem_007", PoseRecord(byRef));

	int32_t adapter = -2, output = -2;
	sys003->GetDXGIOutputInfo(&adapter, &output);
	Json::Value dxgi;
	dxgi["output"] = output;
	dxgi["adapter_set"] = adapter != -2;
	c.Record("GetDXGIOutputInfo IVRSystem_003", dxgi);

	Json::Value attach;
	attach["ret"] = sys003->AttachToWindow(nullptr);
	c.Record("AttachToWindow", attach);

	vr::IVRSystem_003::RenderModel_t model;
	memset(&model, 0xCD, sizeof(model));
	Json::Value load;
	load["ret"] = sys003->LoadRenderModel("renderLeftHand", &model);
	load["has_vertices"] = model.unVertexCount > 0 && model.rVertexData != nullptr;
	load["has_triangles"] = model.unTriangleCount > 0 && model.rIndexData != nullptr;
	load["texture"] = std::to_string(model.diffuseTexture.unWidth) + "x" + std::to_string(model.diffuseTexture.unHeight);
	load["has_texture_data"] = model.diffuseTexture.rubTextureMapData != nullptr;
	c.Record("LoadRenderModel renderLeftHand", load);
	sys003->FreeRenderModel(&model);
	Json::Value freed;
	freed["handle"] = (Json::UInt64)model.ulInternalHandle;
	freed["vertices"] = model.unVertexCount;
	c.Record("FreeRenderModel", freed);
	sys003->FreeRenderModel(&model); // again: ignored

	memset(&model, 0xCD, sizeof(model));
	Json::Value unknown;
	unknown["ret"] = sys003->LoadRenderModel("no such model", &model);
	unknown["handle"] = (Json::UInt64)model.ulInternalHandle;
	c.Record("LoadRenderModel unknown", unknown);

	// IVRTrackedCamera_001 in SDK 0.9.12's slot order, which OpenLamina answers (0.9.11 has fewer slots, 0.9.14 one
	// gone from the middle), through the last slot: no camera, so false, 0 or null, and the outputs left as they were
	auto* cam = (vr::IVRTrackedCamera_001::IVRTrackedCamera*)c.core->GetGenericInterface("IVRTrackedCamera_001", &le);
	Json::Value camera;
	camera["found"] = cam != nullptr;
	if (cam) {
		char firmware[16];
		memset(firmware, 'x', sizeof(firmware));
		firmware[sizeof(firmware) - 1] = 0;
		uint32_t width = 7, height = 7;
		float u = 7, v = 7;
		vr::HmdMatrix44_t projection;
		memset(&projection, 0xCD, sizeof(projection));
		camera["HasCamera"] = cam->HasCamera(0);
		camera["GetCameraFirmwareDescription"] = cam->GetCameraFirmwareDescription(0, firmware, sizeof(firmware));
		camera["firmware"] = firmware;
		camera["GetCameraFrameDimensions"] = cam->GetCameraFrameDimensions(0, (vr::ECameraVideoStreamFormat)1, &width, &height);
		camera["size"] = std::to_string(width) + "x" + std::to_string(height);
		camera["GetCameraVideoStreamFormat"] = (int)cam->GetCameraVideoStreamFormat(0);
		camera["GetVideoStreamElapsedTime"] = cam->GetVideoStreamElapsedTime(0);
		camera["GetVideoStreamFrame"] = cam->GetVideoStreamFrame(0) == nullptr ? "null" : "set";
		camera["SupportsPauseResume"] = cam->SupportsPauseResume(0);
		camera["IsVideoStreamPaused"] = cam->IsVideoStreamPaused(0);
		camera["GetCameraDistortion"] = cam->GetCameraDistortion(0, 0.5f, 0.5f, &u, &v);
		camera["distortion"] = std::to_string(u) + "," + std::to_string(v);
		camera["GetCameraProjection"] = cam->GetCameraProjection(0, 640, 480, 0.1f, 100, &projection);
		camera["projection_zeroed"] = projection.m[0][0] == 0 && projection.m[3][3] == 0;
	}
	c.Record("IVRTrackedCamera_001", camera);

	c.core->Cleanup();
}

// The old compositors' Submit forms on a D3D11 texture (expect/old_submit.json): IVRCompositor_005 and 006 name the
// device with SetGraphicsDevice, 008 passes the API with each texture. And GetFrameTiming writes only the old struct.
void OldSubmit(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* c005 = (vr::IVRCompositor_005::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_005", &le);
	auto* c006 = (vr::IVRCompositor_006::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_006", &le);
	auto* c008 = (vr::IVRCompositor_008::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_008", &le);
	if (!c005 || !c006 || !c008) {
		c.Record("no IVRCompositor_005, 006 or 008", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	void* handle = tex.get() ? tex.get()->handle : nullptr;
	vr::TrackedDevicePose_t poses[16];

	// 008: the API with each texture
	Json::Value r;
	r["wait"] = (int)c008->WaitGetPoses(poses, 16, nullptr, 0);
	r["left"] = (int)c008->Submit(vr::Eye_Left, API_DirectX, handle, nullptr, vr::Submit_Default);
	r["right"] = (int)c008->Submit(vr::Eye_Right, API_DirectX, handle, nullptr, vr::Submit_Default);
	c.Record("IVRCompositor_008 frame", r);

	// 006: no device named yet, so the texture can't be read; then D3D11
	r = Json::Value();
	r["wait"] = (int)c006->WaitGetPoses(poses, 16, nullptr, 0);
	r["left before SetGraphicsDevice"] = (int)c006->Submit(vr::Eye_Left, handle, nullptr);
	c006->SetGraphicsDevice(vr::IVRCompositor_006::Compositor_DeviceType_D3D11, c.call->d3d11);
	r["left"] = (int)c006->Submit(vr::Eye_Left, handle, nullptr);
	r["right"] = (int)c006->Submit(vr::Eye_Right, handle, nullptr);
	c.Record("IVRCompositor_006 frame", r);

	// 005: the same, with its own bounds struct and no answers; a frame after it still works
	c005->SetGraphicsDevice(vr::IVRCompositor_005::Compositor_DeviceType_D3D11, c.call->d3d11);
	c005->WaitGetPoses(poses, 16);
	vr::IVRCompositor_005::Compositor_TextureBounds bounds = { 0, 0, 1, 1 };
	c005->Submit(vr::Eye_Left, handle, &bounds);
	c005->Submit(vr::Eye_Right, handle, &bounds);
	r = Json::Value();
	r["wait after"] = (int)f.comp->WaitGetPoses(poses, 16, nullptr, 0);
	r["left after"] = (int)f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
	c.Record("IVRCompositor_005 frame", r);

	// Frame timing in 005's and 008's own layouts: nothing written past the struct
	alignas(8) uint8_t buffer[512];
	memset(buffer, 0xCD, sizeof(buffer));
	auto* t005 = (vr::IVRCompositor_005::Compositor_FrameTiming*)buffer;
	t005->size = sizeof(*t005);
	r = Json::Value();
	r["ret"] = c005->GetFrameTiming(t005, 0);
	int overrun = 0;
	for (size_t i = sizeof(*t005); i < sizeof(buffer); i++)
		overrun += buffer[i] != 0xCD;
	r["overrun"] = overrun;
	r["size kept"] = t005->size == sizeof(*t005);
	c.Record("IVRCompositor_005 GetFrameTiming", r);

	memset(buffer, 0xCD, sizeof(buffer));
	auto* t008 = (vr::IVRCompositor_008::Compositor_FrameTiming*)buffer;
	t008->size = sizeof(*t008);
	r = Json::Value();
	r["ret"] = c008->GetFrameTiming(t008, 0);
	overrun = 0;
	for (size_t i = sizeof(*t008); i < sizeof(buffer); i++)
		overrun += buffer[i] != 0xCD;
	r["overrun"] = overrun;
	r["size kept"] = t008->size == sizeof(*t008);
	c.Record("IVRCompositor_008 GetFrameTiming", r);

	c.core->Cleanup();
}

// An event polled into SDK 0.9.x's 32-byte VREvent_t: its type, the keyboard user value at its data's offset 8 (16), and
// how many bytes past 32 were written
Json::Value OldEventRecord(bool polled, const uint8_t* buffer)
{
	Json::Value r;
	r["polled"] = polled;
	uint32_t type;
	uint64_t userValue;
	memcpy(&type, buffer, sizeof(type));
	memcpy(&userValue, buffer + 16, sizeof(userValue));
	r["type"] = (int)type;
	r["user value"] = (Json::UInt64)userValue;
	int overrun = 0;
	for (int i = 32; i < 64; i++)
		overrun += buffer[i] != 0xCD;
	r["overrun"] = overrun;
	return r;
}

// The old overlay calls (expect/old_overlay.json): events in the old layout, flags as bit numbers (up to 020) and as
// masks (021 on), the 0.9.x gamma and visibility calls, and 001's texture of the device SetGraphicsDevice named
void OldOverlay(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	auto* ov007 = (vr::IVROverlay_007::IVROverlay*)c.core->GetGenericInterface("IVROverlay_007", &le);
	auto* ov006 = (vr::IVROverlay_006::IVROverlay*)c.core->GetGenericInterface("IVROverlay_006", &le);
	auto* ov001 = (vr::IVROverlay_001::IVROverlay*)c.core->GetGenericInterface("IVROverlay_001", &le);
	auto* sys009 = (vr::IVRSystem_009::IVRSystem*)c.core->GetGenericInterface("IVRSystem_009", &le);
	if (!ov || !ov007 || !ov006 || !ov001 || !sys009) {
		c.Record("no IVROverlay_028, 007, 006, 001 or IVRSystem_009", Err(le));
		c.core->Cleanup();
		return;
	}
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	c.Record("CreateOverlay", Err(ov->CreateOverlay("oc.old", "Old", &h)));

	// Keyboard events, typed with the laser (scripts/keyboard.json, ini/keyboard_us.ini; the old input read from frame
	// 2 loads the actions it reads): the first character (H) polled through 007 from the overlay's keyboard, then the
	// next (i) through IVRSystem_009 from the system's, shown once the first has come. Bounded by 80 frames.
	alignas(8) uint8_t buffer[64];
	callers::Texture frameTex(ArgSet::Valid, *c.call);
	Json::Value overlayEvent, systemEvent;
	for (int frame = 1; frame <= 80 && systemEvent.isNull(); frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		f.comp->Submit(vr::Eye_Left, frameTex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, frameTex.get(), nullptr, vr::Submit_Default);
		if (frame >= 2) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(2, &state, sizeof(state));
		}
		if (frame == 4)
			ov007->ShowKeyboardForOverlay(h, vr::IVROverlay_007::k_EGamepadTextInputModeNormal, vr::IVROverlay_007::k_EGamepadTextInputLineModeSingleLine,
			    "", 8, "", false, 0x1122334455667788ull);
		if (overlayEvent.isNull()) {
			memset(buffer, 0xCD, sizeof(buffer));
			if (ov007->PollNextOverlayEvent(h, (vr::VREvent_t*)buffer)) {
				overlayEvent = OldEventRecord(true, buffer);
				ov007->HideKeyboard();
				ov007->ShowKeyboard(vr::IVROverlay_007::k_EGamepadTextInputModeNormal, vr::IVROverlay_007::k_EGamepadTextInputLineModeSingleLine, "", 8, "",
				    false, 0x55aaull);
			}
			continue;
		}
		// The system's other events first: the first keyboard one is recorded
		for (int i = 0; i < 64; i++) {
			memset(buffer, 0xCD, sizeof(buffer));
			if (!sys009->PollNextEvent((vr::VREvent_t*)buffer))
				break;
			uint32_t type;
			memcpy(&type, buffer, sizeof(type));
			if (type >= vr::VREvent_KeyboardClosed && type <= vr::VREvent_KeyboardDone) {
				systemEvent = OldEventRecord(true, buffer);
				break;
			}
		}
	}
	ov007->HideKeyboard();
	memset(buffer, 0, sizeof(buffer));
	memset(buffer + 32, 0xCD, 32);
	c.Record("IVROverlay_007 PollNextOverlayEvent", overlayEvent.isNull() ? OldEventRecord(false, buffer) : overlayEvent);
	c.Record("IVRSystem_009 PollNextEvent", systemEvent.isNull() ? OldEventRecord(false, buffer) : systemEvent);

	// Flags: 007 passes NoDashboardTab as 3, 028 as 1 << 3; both are bit 3 of GetOverlayFlags
	Json::Value flags;
	flags["set 007"] = (int)ov007->SetOverlayFlag(h, (vr::IVROverlay_007::VROverlayFlags)3, true);
	uint32_t all = 0;
	ov->GetOverlayFlags(h, &all);
	flags["after 007"] = all;
	ov007->SetOverlayFlag(h, (vr::IVROverlay_007::VROverlayFlags)3, false);
	flags["set 028"] = (int)ov->SetOverlayFlag(h, vr::IVROverlay_028::VROverlayFlags_SideBySide_Parallel, true);
	bool enabled = false;
	ov->GetOverlayFlag(h, vr::IVROverlay_028::VROverlayFlags_SideBySide_Parallel, &enabled);
	flags["028 enabled"] = enabled;
	all = 0;
	ov->GetOverlayFlags(h, &all);
	flags["after 028"] = all;
	enabled = false;
	ov007->GetOverlayFlag(h, (vr::IVROverlay_007::VROverlayFlags)10, &enabled); // SideBySide_Parallel's bit number
	flags["007 sees it"] = enabled;
	c.Record("flags", flags);

	Json::Value gamma;
	gamma["set"] = (int)ov006->SetOverlayGamma(h, 0.5f);
	float got = -1;
	gamma["get"] = (int)ov006->GetOverlayGamma(h, &got);
	gamma["value"] = got;
	c.Record("IVROverlay_006 gamma", gamma);

	Json::Value vis;
	vis["set"] = (int)ov001->SetOverlayVisibility(h, vr::IVROverlay_001::VROverlayVisibility_SystemOverlay);
	vr::IVROverlay_001::VROverlayVisibility v = vr::IVROverlay_001::VROverlayVisibility_Manual;
	vis["get"] = (int)ov001->GetOverlayVisibility(h, &v);
	vis["value"] = (int)v;
	vis["system overlay visible"] = ov001->IsSystemOverlayVisible();
	c.Record("IVROverlay_001 visibility", vis);

	// No IVRCompositor::SetGraphicsDevice: 001 can't tell what the texture is
	Json::Value tex;
	tex["ret"] = (int)ov001->SetOverlayTexture(h, nullptr);
	c.Record("IVROverlay_001 SetOverlayTexture", tex);

	ov->DestroyOverlay(h);
	c.core->Cleanup();
}

// The remaining old versions (expect/old_misc.json): IVRApplications_001, IVRChaperone_002, IVRRenderModels_001,
// IVRNotifications_001, IVRControlPanel_001 and IVRInput_003
void OldMisc(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* apps = (vr::IVRApplications_001::IVRApplications*)c.core->GetGenericInterface("IVRApplications_001", &le);
	auto* chap = (vr::IVRChaperone_002::IVRChaperone*)c.core->GetGenericInterface("IVRChaperone_002", &le);
	auto* chap4 = (vr::IVRChaperone_004::IVRChaperone*)c.core->GetGenericInterface("IVRChaperone_004", &le);
	auto* models = (vr::IVRRenderModels_001::IVRRenderModels*)c.core->GetGenericInterface("IVRRenderModels_001", &le);
	auto* notes = (vr::IVRNotifications_001::IVRNotifications*)c.core->GetGenericInterface("IVRNotifications_001", &le);
	auto* panel = (vr::IVRControlPanel_001::IVRControlPanel*)c.core->GetGenericInterface("IVRControlPanel_001", &le);
	auto* input = (vr::IVRInput_003::IVRInput*)c.core->GetGenericInterface("IVRInput_003", &le);
	void* sys = c.core->GetGenericInterface("IVRSystem_022", &le);
	if (!apps || !chap || !chap4 || !models || !notes || !panel || !input || !sys) {
		c.Record("an old interface is missing", Err(le));
		c.core->Cleanup();
		return;
	}

	Json::Value r;
	char key[64] = "x";
	r["ret"] = (int)apps->GetHomeApplication(key, sizeof(key));
	r["key"] = key;
	c.Record("IVRApplications_001 GetHomeApplication", r);

	r = Json::Value();
	vr::IVRChaperone_002::ChaperoneSoftBoundsInfo_t soft = {};
	vr::HmdQuad_t rect = {};
	r["ret"] = chap->GetSoftBoundsInfo(&soft);
	r["play area ret"] = chap4->GetPlayAreaRect(&rect);
	r["same as play area"] = memcmp(&soft.quadCorners, &rect, sizeof(rect)) == 0;
	vr::IVRChaperone_002::ChaperoneSeatedBoundsInfo_t seated;
	memset(&seated, 0xCD, sizeof(seated));
	r["seated ret"] = chap->GetSeatedBoundsInfo(&seated);
	r["seated zeroed"] = seated.vSeatedHeadPosition.v[0] == 0.0f;
	c.Record("IVRChaperone_002 bounds", r);

	r = Json::Value();
	vr::IVRRenderModels_001::RenderModel_t model;
	memset(&model, 0xCD, sizeof(model));
	r["ret"] = models->LoadRenderModel("renderLeftHand", &model);
	r["has_vertices"] = model.unVertexCount > 0 && model.rVertexData != nullptr;
	r["texture"] = std::to_string(model.diffuseTexture.unWidth) + "x" + std::to_string(model.diffuseTexture.unHeight);
	models->FreeRenderModel(&model);
	r["freed handle"] = (Json::UInt64)model.ulInternalHandle;
	c.Record("IVRRenderModels_001 LoadRenderModel", r);

	r = Json::Value();
	char name[64] = {};
	r["size"] = notes->GetErrorString(vr::IVRNotifications_001::k_ENotificationError_Fail, name, sizeof(name));
	r["name"] = name;
	vr::IVRNotifications_001::VRNotificationId id = 0;
	r["create"] = (int)notes->CreateNotification(vr::k_ulOverlayHandleInvalid, 0, "type", "text", "category", nullptr, &id);
	r["id set"] = id != 0;
	r["dismiss"] = (int)notes->DismissNotification(id);
	c.Record("IVRNotifications_001", r);

	r = Json::Value();
	r["drivers"] = panel->GetDriverCount();
	r["display interface"] = (void*)panel->GetCurrentDisplayInterface("IVRSystem_022") == sys;
	r["unknown interface null"] = panel->GetCurrentDisplayInterface("IVRNoSuchThing_001") == nullptr;
	r["ipd positive"] = panel->GetIPD() > 0.0f;
	c.Record("IVRControlPanel_001", r);

	// IVRInput_003's action sets are 24 bytes (today's 32)
	r = Json::Value();
	vr::IVRInput_003::VRActiveActionSet_t set = {};
	r["24 bytes"] = (int)input->UpdateActionState(&set, 24, 0);
	r["32 bytes"] = (int)input->UpdateActionState(&set, 32, 0);
	c.Record("IVRInput_003 UpdateActionState", r);

	c.core->Cleanup();
}

// The interfaces the fork never had (expect/new_interfaces.json): an anchor kept and given back through its descriptor,
// the one driver (the OpenXR runtime), and no camera
void NewInterfaces(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* anchors = (vr::IVRSpatialAnchors_001::IVRSpatialAnchors*)c.core->GetGenericInterface("IVRSpatialAnchors_001", &le);
	auto* drivers = (vr::IVRDriverManager_001::IVRDriverManager*)c.core->GetGenericInterface("IVRDriverManager_001", &le);
	auto* camera = (vr::IVRTrackedCamera_006::IVRTrackedCamera*)c.core->GetGenericInterface("IVRTrackedCamera_006", &le);
	if (!anchors || !drivers || !camera) {
		c.Record("a new interface is missing", Err(le));
		c.core->Cleanup();
		return;
	}

	vr::IVRSpatialAnchors_001::SpatialAnchorPose_t pose = { { { { 1, 0, 0, 0.5f }, { 0, 1, 0, 1.25f }, { 0, 0, 1, -2 } } } };
	vr::SpatialAnchorHandle_t handle = 0;
	Json::Value r;
	r["create"] = (int)anchors->CreateSpatialAnchorFromPose(0, vr::TrackingUniverseStanding, &pose, &handle);
	vr::IVRSpatialAnchors_001::SpatialAnchorPose_t back = {};
	r["get"] = (int)anchors->GetSpatialAnchorPose(handle, vr::TrackingUniverseStanding, &back);
	r["same pose"] = memcmp(&back, &pose, sizeof(pose)) == 0;
	uint32_t length = 0;
	r["descriptor size asked"] = (int)anchors->GetSpatialAnchorDescriptor(handle, nullptr, &length);
	std::string descriptor(length, '\0');
	r["descriptor"] = (int)anchors->GetSpatialAnchorDescriptor(handle, descriptor.data(), &length);
	vr::SpatialAnchorHandle_t again = 0;
	r["from descriptor"] = (int)anchors->CreateSpatialAnchorFromDescriptor(descriptor.c_str(), &again);
	back = {};
	anchors->GetSpatialAnchorPose(again, vr::TrackingUniverseStanding, &back);
	r["descriptor pose same"] = memcmp(&back, &pose, sizeof(pose)) == 0;
	r["new handle"] = again != handle && again != 0;
	r["unknown handle"] = (int)anchors->GetSpatialAnchorPose(handle + 1000, vr::TrackingUniverseStanding, &back);
	c.Record("IVRSpatialAnchors_001 round trip", r);

	r = Json::Value();
	char name[64] = {};
	r["count"] = drivers->GetDriverCount();
	r["name size"] = drivers->GetDriverName(0, name, sizeof(name));
	r["name"] = name;
	r["handle"] = (Json::UInt64)drivers->GetDriverHandle(name);
	r["enabled"] = drivers->IsEnabled(0);
	c.Record("IVRDriverManager_001", r);

	r = Json::Value();
	bool has = true;
	r["ret"] = (int)camera->HasCamera(vr::k_unTrackedDeviceIndex_Hmd, &has);
	r["has camera"] = has;
	c.Record("IVRTrackedCamera_006 HasCamera", r);

	// IVRPaths through the C API, as Valve offers it: openvr_capi.h's VR_IVRPaths_FnTable
#ifdef _WIN32
#define OC_FNTABLE __stdcall
#else
#define OC_FNTABLE
#endif
	using namespace vr::IVRPaths_002;
	struct PathsFnTable {
		vr::ETrackedPropertyError(OC_FNTABLE* ReadPathBatch)(vr::PropertyContainerHandle_t, PathRead_t*, uint32_t);
		vr::ETrackedPropertyError(OC_FNTABLE* WritePathBatch)(vr::PropertyContainerHandle_t, PathWrite_t*, uint32_t);
		vr::ETrackedPropertyError(OC_FNTABLE* StringToHandle)(PathHandle_t*, const char*);
		vr::ETrackedPropertyError(OC_FNTABLE* HandleToString)(PathHandle_t, char*, uint32_t, uint32_t*);
	};
#undef OC_FNTABLE
	auto* paths = (PathsFnTable*)c.core->GetGenericInterface("FnTable:IVRPaths_002", &le);
	r = Json::Value();
	r["found"] = paths != nullptr;
	if (paths) {
		PathHandle_t first = 0, second = 0;
		r["to handle"] = (int)paths->StringToHandle(&first, "/user/head/proximity");
		paths->StringToHandle(&second, "/user/head/proximity");
		r["same handle"] = first == second && first != 0;
		char path[64] = {};
		uint32_t used = 0;
		r["to string"] = (int)paths->HandleToString(first, path, sizeof(path), &used);
		r["path"] = path;
		r["used"] = used;
		r["small buffer"] = (int)paths->HandleToString(first, path, 4, &used);
		PathRead_t read[2] = {};
		read[0].ulPath = first;
		read[1].ulPath = first;
		r["read"] = (int)paths->ReadPathBatch(0, read, 2);
		r["entry errors"] = (int)read[0].eError * 10 + (int)read[1].eError;
		PathWrite_t write[2] = {};
		write[1].bValueChanged = true; // 002's last field, past 001's struct: the stride is 002's
		r["write"] = (int)paths->WritePathBatch(0, write, 2);
		r["write entry errors"] = (int)write[0].eError * 10 + (int)write[1].eError;
		r["stride kept"] = write[1].bValueChanged;
	}
	c.Record("FnTable:IVRPaths_002", r);

	c.core->Cleanup();
}

// What the devices say they are (expect/device_identity.json): the HMD's row in the identity table, a string answer
// that doesn't fit, and IVRSystem_003's distortion
void DeviceIdentityCase(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	auto* sys003 = (vr::IVRSystem_003::IVRSystem*)c.core->GetGenericInterface("IVRSystem_003", &le);
	if (!sys || !sys003) {
		c.Record("no IVRSystem_022 or 003", Err(le));
		c.core->Cleanup();
		return;
	}
	auto stringProp = [&](const char* label, vr::ETrackedDeviceProperty prop, uint32_t size) {
		char value[64];
		memset(value, 'x', sizeof(value));
		vr::ETrackedPropertyError err = vr::TrackedProp_Success;
		Json::Value r;
		r["ret"] = sys->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, prop, value, size, &err);
		r["error"] = (int)err;
		r["value"] = size ? std::string(value, strnlen(value, size)) : std::string();
		c.Record(label, r);
	};
	stringProp("HMD TrackingSystemName", vr::Prop_TrackingSystemName_String, 64);
	stringProp("HMD ManufacturerName", vr::Prop_ManufacturerName_String, 64);
	stringProp("HMD RenderModelName", vr::Prop_RenderModelName_String, 64);
	stringProp("HMD ControllerType", vr::Prop_ControllerType_String, 64);
	stringProp("HMD TrackingSystemName in 3 bytes", vr::Prop_TrackingSystemName_String, 3);

	Json::Value r;
	vr::DistortionCoordinates_t d = sys003->ComputeDistortion(vr::Eye_Left, 0.25f, 0.75f);
	r["red"] = std::to_string(d.rfRed[0]) + "," + std::to_string(d.rfRed[1]);
	r["blue"] = std::to_string(d.rfBlue[0]) + "," + std::to_string(d.rfBlue[1]);
	c.Record("IVRSystem_003 ComputeDistortion", r);

	c.core->Cleanup();
}

// What a program that only watches the VR system (the Steam client) does to the OpenXR runtime. Run as steam.exe and
// as oc_conformance.exe (expect/observer_steam.json, expect/observer_game.json) against the stand-in runtime, which
// writes its dump (OC_TEST_RUNTIME_DUMP) when an instance is destroyed: the dump existing is the runtime having been
// started.
void ObserverCase(CaseContext& c)
{
	Json::Value program;
#ifdef _WIN32
	wchar_t path[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
	program["program"] = std::filesystem::path(std::wstring(path, n)).filename().string();
#endif
	c.Record("program", program);

	Json::Value init;
	init["init_error"] = (int)c.core->Init(vr::VRApplication_Background, nullptr);
	c.Record("Init(Background)", init);

	Json::Value present;
	present["ret"] = c.core->BIsHmdPresent();
	c.Record("BIsHmdPresent", present);

	Json::Value started;
	const char* dump = getenv("OC_TEST_RUNTIME_DUMP");
	std::error_code ec;
	started["runtime_started"] = dump && std::filesystem::exists(dump, ec);
	c.Record("OpenXR runtime", started);
	if (init["init_error"].asInt() == vr::VRInitError_None)
		c.core->Cleanup();
}

// This program's folder, where oc_echo_app is built
std::filesystem::path OwnFolder()
{
#ifdef _WIN32
	wchar_t path[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof(path) / sizeof(path[0])));
	return std::filesystem::path(std::wstring(path, n)).parent_path();
#else
	std::error_code ec;
	return std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
#endif
}

std::string JsonEscaped(const std::string& s)
{
	std::string out;
	for (char ch : s) {
		if (ch == '\\' || ch == '"')
			out += '\\';
		out += ch;
	}
	return out;
}

// The application registry (expect/applications.json): a manifest added, its application installed, launched
// (oc_echo_app writes a marker file) and named. A Utility app, so it runs without an OpenXR runtime too.
void ApplicationsCase(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Utility, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* apps = (vr::IVRApplications_007::IVRApplications*)c.core->GetGenericInterface("IVRApplications_007", &le);
	if (!apps) {
		c.Record("no IVRApplications_007", Err(le));
		c.core->Cleanup();
		return;
	}

	// echo.vrmanifest, with the binary where it was built and the marker to write as its argument
	namespace fs = std::filesystem;
	auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	fs::path dir = fs::temp_directory_path() / ("oc_applications_case_" + std::to_string(stamp));
	fs::create_directories(dir);
	fs::path marker = dir / "marker.txt";
	std::ifstream in(c.call->dataDir / "echo.vrmanifest");
	std::stringstream text;
	text << in.rdbuf();
	std::string manifest = text.str();
#ifdef _WIN32
	const char* binaryKey = "\"binary_path_windows\" : \"oc_echo_app.exe\"";
	fs::path binary = OwnFolder() / "oc_echo_app.exe";
#else
	const char* binaryKey = "\"binary_path_linux\" : \"oc_echo_app\"";
	fs::path binary = OwnFolder() / "oc_echo_app";
#endif
	auto replace = [&](const std::string& from, const std::string& to) {
		size_t at = manifest.find(from);
		if (at != std::string::npos)
			manifest.replace(at, from.size(), to);
	};
	std::string key = binaryKey;
	replace(key, key.substr(0, key.find(':') + 2) + "\"" + JsonEscaped(binary.generic_string()) + "\"");
	replace("\"arguments\" : \"\"", "\"arguments\" : \"\\\"" + JsonEscaped(marker.generic_string()) + "\\\" launched\"");
	fs::path manifestPath = dir / "echo.vrmanifest";
	std::ofstream(manifestPath) << manifest;

	c.Record("AddApplicationManifest", Err(apps->AddApplicationManifest(manifestPath.string().c_str(), false)));
	Json::Value installed;
	installed["ret"] = apps->IsApplicationInstalled("oc.test.echo");
	c.Record("IsApplicationInstalled", installed);

	Json::Value launch = Err(apps->LaunchApplication("oc.test.echo"));
	bool appeared = false;
	for (int i = 0; i < 50 && !appeared; i++) {
		std::error_code ec;
		appeared = fs::exists(marker, ec) && fs::file_size(marker, ec) > 0;
		if (!appeared)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	launch["marker"] = appeared;
	if (appeared) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // its one line written
		std::ifstream m(marker);
		std::string line;
		std::getline(m, line);
		launch["marker_text"] = line;
	}
	c.Record("LaunchApplication", launch);

	char name[64] = "unchanged";
	vr::IVRApplications_007::EVRApplicationError error = vr::IVRApplications_007::VRApplicationError_None;
	Json::Value prop;
	prop["ret"] = apps->GetApplicationPropertyString("oc.test.echo", vr::IVRApplications_007::VRApplicationProperty_Name_String, name, sizeof(name), &error);
	prop["error"] = (int)error;
	prop["value"] = std::string(name);
	c.Record("GetApplicationPropertyString Name", prop);

	Json::Value last;
	last["after_launch"] = apps->GetApplicationPropertyUint64("oc.test.echo", vr::IVRApplications_007::VRApplicationProperty_LastLaunchTime_Uint64, &error) > 0;
	c.Record("LastLaunchTime", last);

	c.Record("RemoveApplicationManifest", Err(apps->RemoveApplicationManifest(manifestPath.string().c_str())));
	Json::Value gone;
	gone["ret"] = apps->IsApplicationInstalled("oc.test.echo");
	c.Record("IsApplicationInstalled after removal", gone);

	c.core->Cleanup();
	std::error_code ec;
	fs::remove_all(dir, ec);
}

// The room setup (expect/chaperone.json), on the stand-in with stage_bounds [2, 3]
// (scripts/stage_2x3.json). "chaperone" commits a 4 x 5 play area; "chaperone-read", run after it in another process
// (run.py --jobs 1), finds it.
vr::IVRChaperoneSetup_006::IVRChaperoneSetup* OpenChaperoneSetup(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return nullptr;
	}
	vr::EVRInitError le;
	auto* setup = (vr::IVRChaperoneSetup_006::IVRChaperoneSetup*)c.core->GetGenericInterface("IVRChaperoneSetup_006", &le);
	if (!setup) {
		c.Record("no IVRChaperoneSetup_006", Err(le));
		c.core->Cleanup();
	}
	return setup;
}

void RecordPlayArea(CaseContext& c, vr::IVRChaperoneSetup_006::IVRChaperoneSetup* setup, const char* label)
{
	float x = -1, z = -1;
	Json::Value r;
	r["ret"] = setup->GetWorkingPlayAreaSize(&x, &z);
	r["size"] = std::to_string(x) + "," + std::to_string(z);
	c.Record(label, r);
}

void ChaperoneCase(CaseContext& c)
{
	auto* setup = OpenChaperoneSetup(c);
	if (!setup)
		return;

	uint32_t count = 0;
	Json::Value live;
	live["count_ret"] = setup->GetLiveCollisionBoundsInfo(nullptr, &count);
	live["count"] = count;
	vr::HmdQuad_t quad{};
	count = 1;
	live["ret"] = setup->GetLiveCollisionBoundsInfo(&quad, &count);
	std::string corners;
	for (const vr::HmdVector3_t& v : quad.vCorners)
		corners += (corners.empty() ? "" : " ") + std::to_string(v.v[0]) + "," + std::to_string(v.v[2]);
	live["corners"] = corners;
	c.Record("GetLiveCollisionBoundsInfo", live);

	RecordPlayArea(c, setup, "working play area before");
	setup->SetWorkingPlayAreaSize(4, 5);
	RecordPlayArea(c, setup, "working play area set");
	Json::Value commit;
	commit["ret"] = setup->CommitWorkingCopy(vr::IVRChaperoneSetup_006::EChaperoneConfigFile_Live);
	c.Record("CommitWorkingCopy", commit);
	c.core->Cleanup();
}

void ChaperoneReadCase(CaseContext& c)
{
	auto* setup = OpenChaperoneSetup(c);
	if (!setup)
		return;
	RecordPlayArea(c, setup, "working play area in the next process");

	// Exported, changed, and imported back into the working copy
	uint32_t size = 0;
	Json::Value exported;
	exported["size_ret"] = setup->ExportLiveToBuffer(nullptr, &size);
	std::string text(size, '\0');
	exported["ret"] = size > 0 && setup->ExportLiveToBuffer(text.data(), &size);
	c.Record("ExportLiveToBuffer", exported);
	setup->SetWorkingPlayAreaSize(1, 1);
	Json::Value imported;
	imported["ret"] = setup->ImportFromBufferToWorking(text.c_str(), 0);
	c.Record("ImportFromBufferToWorking", imported);
	RecordPlayArea(c, setup, "working play area imported");
	c.core->Cleanup();
}

// To a thousandth, so a record compares exactly
double Thousandths(double v)
{
	double r = std::round(v * 1000) / 1000;
	return r == 0 ? 0.0 : r; // no -0
}

Json::Value ThousandthsOf(const vr::HmdVector3_t& v)
{
	Json::Value out(Json::arrayValue);
	for (float x : v.v)
		out.append(Thousandths(x));
	return out;
}

// The room setup's standing zero pose moved 1 m forward and committed, then put back (expect/chaperone_offsets.json, on
// Monado with libmonado and applyZeroPoses: the stage moves, so the head's standing pose moves 1 m back, as does the
// seated origin in it).
// A simulated head moves by itself, so the head is measured from its seated pose at the same time (the seated space
// doesn't move), and its plain move is recorded as well. Each pose after 30 frames; millimetres.
void ChaperoneOffsetsCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* setup = (vr::IVRChaperoneSetup_006::IVRChaperoneSetup*)c.core->GetGenericInterface("IVRChaperoneSetup_006", &le);
	if (!setup) {
		c.Record("no IVRChaperoneSetup_006", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	struct Where {
		float head[3];
		float headFromSeated[3]; // the head's standing pose less its seated one
		float seated[3];
	};
	auto settle = [&]() {
		vr::TrackedDevicePose_t poses[16]{};
		for (int frame = 1; frame <= 30; frame++) {
			f.comp->WaitGetPoses(poses, 16, nullptr, 0);
			f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
			f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		}
		vr::TrackedDevicePose_t seatedPoses[16]{};
		f.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, 16);
		f.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0, seatedPoses, 16);
		const vr::HmdMatrix34_t seated = f.sys->GetSeatedZeroPoseToStandingAbsoluteTrackingPose();
		Where w;
		for (int i = 0; i < 3; i++) {
			w.head[i] = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[i][3];
			w.headFromSeated[i] = w.head[i] - seatedPoses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[i][3];
			w.seated[i] = seated.m[i][3];
		}
		return w;
	};
	auto moved = [&](const char* label, const Where& from, const Where& to, bool committed) {
		Json::Value r;
		r["committed"] = committed;
		Json::Value head(Json::arrayValue), seated(Json::arrayValue);
		for (int i = 0; i < 3; i++) {
			head.append(Thousandths(to.head[i] - from.head[i]));
			seated.append(Thousandths(to.seated[i] - from.seated[i]));
		}
		r["head moved"] = head;
		r["seated origin moved"] = seated;
		// 1 m back (+Z) within 5 mm
		auto back = [&](const float* a, const float* b) {
			return std::fabs(b[0] - a[0]) < 0.005f && std::fabs(b[1] - a[1]) < 0.005f && std::fabs(b[2] - a[2] - 1) < 0.005f;
		};
		r["head 1 m back"] = back(from.headFromSeated, to.headFromSeated);
		r["seated origin 1 m back"] = back(from.seated, to.seated);
		c.Record(label, r);
	};

	const Where before = settle();
	setup->RevertWorkingCopy();
	vr::HmdMatrix34_t zero{};
	zero.m[0][0] = zero.m[1][1] = zero.m[2][2] = 1;
	zero.m[2][3] = -1;
	setup->SetWorkingStandingZeroPoseToRawTrackingPose(&zero);
	const bool committed = setup->CommitWorkingCopy(vr::IVRChaperoneSetup_006::EChaperoneConfigFile_Live);
	const Where after = settle();
	moved("standing zero 1 m forward", before, after, committed);

	// Put back: the stage where it was
	zero.m[2][3] = 0;
	setup->SetWorkingStandingZeroPoseToRawTrackingPose(&zero);
	const bool restored = setup->CommitWorkingCopy(vr::IVRChaperoneSetup_006::EChaperoneConfigFile_Live);
	const Where back = settle();
	moved("standing zero back", back, after, restored);
	c.core->Cleanup();
}

// A controller's system button held, and pressed (expect/recentre.json, with scripts/recentre.json and
// ini/recentre.ini: dashboardButton=system). The head stands away from the stage's origin, turned; the right system
// button is held from frame 20 to 150 (over a second), then the head moves and the left one is held from frame 180 to
// 310: each hold recentres once (on the stand-in, whose stage can't be moved, the seated origin goes under the head)
// with a pulse on that controller, and opens no dashboard. Then the left button is pressed from frame 340 to 350, which
// opens the dashboard when it is let go, and the right one from 370 to 380, which closes it.
void RecentreCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	vr::EVRInitError le;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	Json::Value events(Json::arrayValue);
	auto head = [&](vr::ETrackingUniverseOrigin origin) {
		vr::TrackedDevicePose_t poses[16]{};
		f.sys->GetDeviceToAbsoluteTrackingPose(origin, 0, poses, 16);
		return poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
	};
	auto position = [](const vr::HmdMatrix34_t& m) {
		Json::Value out(Json::arrayValue);
		for (int i = 0; i < 3; i++)
			out.append(Milli(m.m[i][3]));
		return out;
	};
	// Looking along the origin's -Z, level
	auto facesForward = [](const vr::HmdMatrix34_t& m) {
		return std::fabs(m.m[0][2]) < 0.01f && std::fabs(m.m[1][2]) < 0.01f && m.m[2][2] > 0.99f;
	};
	Json::Value r;
	for (int frame = 1; frame <= 400; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		// The old input read, so that the controllers' actions are there
		vr::VRControllerState_t state{};
		f.sys->GetControllerState(1, &state, sizeof(state));
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		vr::VREvent_t ev{};
		while (f.sys->PollNextEvent(&ev, sizeof(ev))) {
			const char* name = ev.eventType == vr::VREvent_SeatedZeroPoseReset ? "seated zero pose reset"
			    : ev.eventType == vr::VREvent_ChaperoneUniverseHasChanged      ? "chaperone universe changed"
			    : ev.eventType == vr::VREvent_DashboardActivated               ? "dashboard activated"
			    : ev.eventType == vr::VREvent_DashboardDeactivated             ? "dashboard deactivated"
			                                                                   : nullptr;
			if (name && events.size() < 16)
				events.append(name);
		}
		if (frame == 15)
			r["standing_head_before"] = position(head(vr::TrackingUniverseStanding));
		if (frame == 100)
			r["events_before_a_second_held"] = (int)events.size();
		if (frame == 165 || frame == 325) {
			// After each hold: the head is the seated origin, the standing space is as it was, no dashboard
			const char* which = frame == 165 ? "first" : "second";
			const vr::HmdMatrix34_t seated = head(vr::TrackingUniverseSeated);
			Json::Value after;
			after["seated_head"] = position(seated);
			after["seated_head_faces_forward"] = facesForward(seated);
			after["standing_head"] = position(head(vr::TrackingUniverseStanding));
			after["dashboard_open"] = (ov && ov->IsDashboardVisible());
			after["events"] = events;
			r[which] = after;
		}
		if (frame == 360)
			r["dashboard_open_after_press"] = (ov && ov->IsDashboardVisible());
		if (frame == 395)
			r["dashboard_open_after_second_press"] = (ov && ov->IsDashboardVisible());
	}
	r["events"] = events;
	c.core->Cleanup();
	// One pulse on each controller, at its hold
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		Json::Value pulses(Json::arrayValue);
		for (const Json::Value& h : dump["haptics"])
			if (pulses.size() < 16)
				pulses.append(h["action"]);
		r["pulses"] = pulses;
	}
	c.Record("recentre", r);
}

// Events from calls (expect/events.json), on the stand-in with scripts/legacy_click.json: the Index profile's
// right trigger clicked from frame 10 to 20. The game reads the legacy input (GetControllerState at frame 2), so the
// legacy button events come; ResetSeatedZeroPose's event is the next one polled after it.
void EventsCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	int pressFrame = -1, unpressFrame = -1, pressDevice = -1;
	int statePressFirst = -1, statePressLast = -1, statePressFrames = 0; // GetControllerState's trigger
	float axisAtPress = 0;
	uint64_t aButtonMask = 0; // what the A click (frames 25 to 30) sets, from frame 27
	Json::Value reset;
	for (int frame = 1; frame <= 40; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		if (frame >= 2) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(2, &state, sizeof(state));
			if (state.ulButtonPressed & (1ull << vr::k_EButton_SteamVR_Trigger)) {
				if (statePressFirst < 0) {
					statePressFirst = frame;
					axisAtPress = state.rAxis[1].x;
				}
				statePressLast = frame;
				statePressFrames++;
			}
			if (frame == 27)
				aButtonMask = state.ulButtonPressed;
		}
		vr::VREvent_t ev{};
		while (f.sys->PollNextEvent(&ev, sizeof(ev))) {
			if (ev.data.controller.button != vr::k_EButton_SteamVR_Trigger)
				continue;
			if (ev.eventType == vr::VREvent_ButtonPress && pressFrame < 0) {
				pressFrame = frame;
				pressDevice = (int)ev.trackedDeviceIndex;
			}
			if (ev.eventType == vr::VREvent_ButtonUnpress && unpressFrame < 0)
				unpressFrame = frame;
		}
		if (frame == 5) {
			// IVRSystem_022 has it no more: IVRChaperone's
			vr::EVRInitError le;
			auto* chap = (vr::IVRChaperone_004::IVRChaperone*)c.core->GetGenericInterface("IVRChaperone_004", &le);
			if (chap)
				chap->ResetZeroPose(vr::TrackingUniverseSeated);
			reset["polled"] = f.sys->PollNextEvent(&ev, sizeof(ev));
			reset["type"] = (int)ev.eventType;
		}
	}
	c.Record("ResetSeatedZeroPose then PollNextEvent", reset);
	Json::Value buttons;
	buttons["press_frame"] = pressFrame;
	buttons["unpress_frame"] = unpressFrame;
	// The stand-in counts frames per session, and reading the legacy input may restart the session once: a few
	// frames late, then the ten frames the script holds it
	buttons["press_in_frames_10_to_16"] = pressFrame >= 10 && pressFrame <= 16;
	buttons["held_10_frames"] = unpressFrame - pressFrame >= 9 && unpressFrame - pressFrame <= 11;
	buttons["device"] = pressDevice;
	c.Record("trigger button events", buttons);
	// The same click through GetControllerState: the trigger pressed for the ten frames the script holds it (from
	// frame 10, a few frames late if the session restarted), its axis 1 at 1
	Json::Value polled;
	polled["first"] = statePressFirst;
	polled["last"] = statePressLast;
	polled["pressed_from_10_to_16"] = statePressFirst >= 10 && statePressFirst <= 16;
	polled["pressed_10_frames"] = statePressFrames == 10 && statePressLast - statePressFirst == 9;
	polled["trigger_axis_full"] = axisAtPress == 1.0f;
	c.Record("trigger controller state", polled);
	// The A click as the old input shows it, and the buttons the right controller says it has
	Json::Value a;
	a["a_bit"] = (aButtonMask & (1ull << vr::k_EButton_A)) != 0;
	a["grip_bit"] = (aButtonMask & (1ull << vr::k_EButton_Grip)) != 0;
	vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
	a["supported_buttons"] = (Json::UInt64)f.sys->GetUint64TrackedDeviceProperty(2, vr::Prop_SupportedButtons_Uint64, &pe);
	a["axis0_type"] = f.sys->GetInt32TrackedDeviceProperty(2, vr::Prop_Axis0Type_Int32, &pe);
	a["axis1_type"] = f.sys->GetInt32TrackedDeviceProperty(2, vr::Prop_Axis1Type_Int32, &pe);
	a["axis2_type"] = f.sys->GetInt32TrackedDeviceProperty(2, vr::Prop_Axis2Type_Int32, &pe);
	c.Record("a button controller state", a);
	c.core->Cleanup();
}

// Input read from two threads at once (expect/input_threads.json): the game's thread syncs and
// reads the manifest's actions and submits frames (OpenLamina reads the old input after each frame, for its button
// events), while another thread reads the old input and the same action all the time, as Unity's SteamVR_Controller
// and a render thread do. With scripts/input_toggle.json and manifest_toggle.json, under either evaluator: nothing may
// crash, and the toggle still turns on, off and on. The second thread starts after frame start_frame (--case-arg
// start_frame=<n>, default 0: before the first frame, so it reads while the first Submit recreates the session for the
// game's graphics API). With the stand-in, the record also has the action calls OpenLamina made on a session that
// wasn't live (action_calls_without_session).
void InputThreadsCase(CaseContext& c)
{
	using namespace vr::IVRInput_010;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* in = (IVRInput*)c.core->GetGenericInterface("IVRInput_010", &le);
	if (!in) {
		c.Record("no IVRInput_010", Err(le));
		c.core->Cleanup();
		return;
	}
	in->SetActionManifestPath(c.manifest.c_str());
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t toggle = 0, haptic = 0;
	in->GetActionSetHandle("/actions/main", &set);
	in->GetActionHandle("/actions/main/in/toggle", &toggle);
	in->GetActionHandle("/actions/main/out/haptic", &haptic);
	callers::Texture tex(ArgSet::Valid, *c.call);
	const int startFrame = c.IntArg("start_frame", 0);

	std::atomic<bool> done{ false }, start{ false };
	std::atomic<int> polls{ 0 };
	std::thread reader([&] {
		while (!start && !done)
			std::this_thread::yield();
		// As fast as it can (to meet the other thread, also through a session restart's waits), until the game's
		// thread is done, but for at most 30 seconds; haptics (which the stand-in counts) only every 50th pass.
		// run.py's memory limit is the backstop.
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		for (int pass = 0; !done && (pass % 256 != 0 || std::chrono::steady_clock::now() < until); pass++) {
			std::this_thread::yield();
			for (vr::TrackedDeviceIndex_t dev = 1; dev <= 2; dev++) {
				vr::VRControllerState_t state{};
				f.sys->GetControllerState(dev, &state, sizeof(state));
			}
			InputDigitalActionData_t d{};
			in->GetDigitalActionData(toggle, &d, sizeof(d), vr::k_ulInvalidInputValueHandle);
			if (pass % 50 == 0)
				in->TriggerHapticVibrationAction(haptic, 0, 0.01f, 160, 0.5f, vr::k_ulInvalidInputValueHandle);
			polls++;
		}
	});
	int changes = 0;
	bool last = false;
	const int frames = 300;
	// The reader's first pass before the frames that meet it (a busy runner may not schedule it at once)
	auto startReader = [&] {
		start = true;
		for (int wait = 0; polls == 0 && wait < 2000; wait++)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	};
	if (startFrame <= 0)
		startReader();
	for (int frame = 1; frame <= frames; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		VRActiveActionSet_t active{};
		active.ulActionSet = set;
		in->UpdateActionState(&active, sizeof(active), 1);
		InputDigitalActionData_t d{};
		in->GetDigitalActionData(toggle, &d, sizeof(d), vr::k_ulInvalidInputValueHandle);
		changes += (bool)d.bState != last ? 1 : 0;
		last = d.bState;
		vr::VRControllerState_t state{};
		f.sys->GetControllerState(2, &state, sizeof(state));
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		if (frame == startFrame)
			startReader();
	}
	done = true;
	reader.join();
	c.core->Cleanup();
	Json::Value r;
	r["frames"] = frames;
	r["polled"] = polls > 0;
	r["toggle_changes"] = changes;
	r["start_frame"] = startFrame;
	// Input read on a session that was being replaced: on a real runtime, a use after free
	Json::Value dump = ReadStandInDump();
	if (dump.isMember("action_calls_without_session"))
		r["action_calls_without_session"] = dump["action_calls_without_session"];

	c.Record("threads", r);
}

// GetActionBindingInfo (expect/binding_info.json), on the stand-in with scripts/bound_source_odd.json and --manifest
// manifest_toggle.json: an input action is told as the binding file binds it; a vibration from the runtime's bound
// sources, which here include one that isn't an input path, and that one is left out
void BindingInfoCase(CaseContext& c)
{
	using namespace vr::IVRInput_010;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* in = (IVRInput*)c.core->GetGenericInterface("IVRInput_010", &le);
	if (!in) {
		c.Record("no IVRInput_010", Err(le));
		c.core->Cleanup();
		return;
	}
	in->SetActionManifestPath(c.manifest.c_str());
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t toggle = 0;
	in->GetActionSetHandle("/actions/main", &set);
	in->GetActionHandle("/actions/main/in/toggle", &toggle);
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 5; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		VRActiveActionSet_t active{};
		active.ulActionSet = set;
		in->UpdateActionState(&active, sizeof(active), 1);
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}
	InputBindingInfo_t info[4]{};
	uint32_t returned = 99;
	Json::Value r;
	// The vibration is answered from the runtime's bound sources: the real bindings come first, the odd source is
	// the last one the stand-in lists
	vr::VRActionHandle_t haptic = 0;
	in->GetActionHandle("/actions/main/out/haptic", &haptic);
	r["err"] = (int)in->GetActionBindingInfo(haptic, info, sizeof(info[0]), 4, &returned);
	r["returned_at_least_one"] = returned >= 1 && returned <= 4;
	bool oddShown = false;
	for (uint32_t i = 0; i < returned && i < 4; i++)
		oddShown = oddShown || std::string(info[i].rchDevicePathName).find("mnd") != std::string::npos || std::string(info[i].rchModeName).find("1419") != std::string::npos;
	r["odd_source_left_out"] = !oddShown;
	bool noBlank = true; // every entry counted is filled in
	for (uint32_t i = 0; i < returned && i < 4; i++)
		noBlank = noBlank && info[i].rchDevicePathName[0] != 0;
	r["no_blank_entries"] = noBlank;
	// The toggle is answered from the binding file: a toggle_button on the right trigger
	memset(info, 0, sizeof(info));
	returned = 99;
	r["toggle_err"] = (int)in->GetActionBindingInfo(toggle, info, sizeof(info[0]), 4, &returned);
	r["toggle_bindings"] = returned;
	if (returned >= 1) {
		r["device"] = info[0].rchDevicePathName;
		r["input"] = info[0].rchInputPathName;
		r["mode"] = info[0].rchModeName;
		r["slot"] = info[0].rchSlotName;
		r["source_type"] = info[0].rchInputSourceType;
	}
	vr::VRInputValueHandle_t origins[4]{};
	in->GetActionOrigins(set, toggle, origins, 4);
	vr::VRInputValueHandle_t right = 0;
	in->GetInputSourceHandle("/user/hand/right", &right);
	r["origin_is_the_right_hand"] = origins[0] == right && origins[1] == vr::k_ulInvalidInputValueHandle;
	c.Record("odd source", r);
	c.core->Cleanup();
}

// The action manifest's actions read frame by frame (expect/input_evaluator.json), on the stand-in with
// scripts/input_toggle.json and --manifest manifest_toggle.json: a toggle_button on the right trigger, which the script
// clicks at frames 10, 30 and 50, and a vector3 action on the left thumbstick, pushed from frame 40. Records where the
// toggle's state changed (the stand-in counts frames per session, and loading the manifest may restart the session, so
// a few frames late), and the vector3 action's data at the end.
void InputFramesCase(CaseContext& c)
{
	using namespace vr::IVRInput_010;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* in = (IVRInput*)c.core->GetGenericInterface("IVRInput_010", &le);
	if (!in) {
		c.Record("no IVRInput_010", Err(le));
		c.core->Cleanup();
		return;
	}
	in->SetActionManifestPath(c.manifest.c_str());
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t toggle = 0, move3 = 0;
	in->GetActionSetHandle("/actions/main", &set);
	in->GetActionHandle("/actions/main/in/toggle", &toggle);
	in->GetActionHandle("/actions/main/in/move3", &move3);
	callers::Texture tex(ArgSet::Valid, *c.call);
	std::vector<int> changes; // the frames the state changed on
	std::vector<int> changedFlags; // the frames bChanged was true on
	int activeFrames = 0, originFrames = 0;
	bool last = false;
	InputAnalogActionData_t analog{};
	vr::EVRInputError analogErr = vr::VRInputError_None;
	for (int frame = 1; frame <= 70; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		VRActiveActionSet_t active{};
		active.ulActionSet = set;
		in->UpdateActionState(&active, sizeof(active), 1);
		InputDigitalActionData_t d{};
		in->GetDigitalActionData(toggle, &d, sizeof(d), vr::k_ulInvalidInputValueHandle);
		activeFrames += d.bActive ? 1 : 0;
		originFrames += d.activeOrigin != vr::k_ulInvalidInputValueHandle ? 1 : 0;
		if ((bool)d.bState != last)
			changes.push_back(frame);
		if (d.bChanged)
			changedFlags.push_back(frame);
		last = d.bState;
		if (frame == 70)
			analogErr = in->GetAnalogActionData(move3, &analog, sizeof(analog), vr::k_ulInvalidInputValueHandle);
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}
	Json::Value out;
	Json::Value list(Json::arrayValue);
	for (int frame : changes)
		list.append(frame);
	out["changes"] = list;
	Json::Value toggle3;
	toggle3["on_off_on"] = changes.size() == 3;
	toggle3["first_on_in_10_to_16"] = !changes.empty() && changes[0] >= 10 && changes[0] <= 16;
	toggle3["twenty_frames_apart"] = changes.size() == 3 && changes[1] - changes[0] == 20 && changes[2] - changes[1] == 20;
	toggle3["changed_only_on_changes"] = changedFlags == changes;
	toggle3["active_every_frame"] = activeFrames == 70;
	toggle3["origin_every_frame"] = originFrames == 70;
	out["checks"] = toggle3;
	// The checks at the top for --expect; all of it as "out", which --expected compares (baseline/input-direct.json)
	Json::Value record = toggle3;
	record["out"] = out;
	c.Record("toggle", record);
	Json::Value v3;
	v3["err"] = (int)analogErr;
	v3["active"] = (bool)analog.bActive;
	v3["x"] = analog.x;
	v3["y"] = analog.y;
	v3["z"] = analog.z;
	c.Record("vector3", v3);
	c.core->Cleanup();
}

// The eye-tracked foveation calls (expect/eye_gaze*.json), on the stand-in with scripts/eye_gaze*.json: the gaze 10
// degrees right (and 5 up, unless canted), tracked until frame 40 of the session, valid but not tracked until frame
// 60, then not valid. Loads --manifest and updates its actions each frame if given, else reads the legacy input. Asks
// at frames 30, 55 and 75 (loading the manifest may restart the session, so a few frames later in its count): each
// eye's centre, and the centre through the left eye's projection, a Y-flipped copy of it and a projection of the
// game's own (a narrower one). Values are rounded to 4 places.
void EyeGazeCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* sys = (vr::IVRSystem_026::IVRSystem*)c.core->GetGenericInterface("IVRSystem_026", &le);
	auto* in = (vr::IVRInput_010::IVRInput*)c.core->GetGenericInterface("IVRInput_010", &le);
	if (!sys || !in) {
		c.Record("no IVRSystem_026 or IVRInput_010", Err(le));
		c.core->Cleanup();
		return;
	}
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	if (!c.manifest.empty()) {
		in->SetActionManifestPath(c.manifest.c_str());
		in->GetActionSetHandle("/actions/main", &set);
	}
	auto round4 = [](float v) { return std::round((double)v * 10000) / 10000; };
	auto ndc = [&](const vr::HmdVector2_t& v) {
		Json::Value a(Json::arrayValue);
		a.append(round4(v.v[0]));
		a.append(round4(v.v[1]));
		return a;
	};
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 75; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		if (!c.manifest.empty()) {
			vr::IVRInput_010::VRActiveActionSet_t active{};
			active.ulActionSet = set;
			in->UpdateActionState(&active, sizeof(active), 1);
		} else {
			vr::VRControllerState_t state{};
			sys->GetControllerState(1, &state, sizeof(state));
		}
		if (frame == 30 || frame == 55 || frame == 75) {
			std::string when = frame == 30 ? "tracked" : frame == 55 ? "not tracked" : "invalid";
			vr::HmdVector2_t left = { { 9, 9 } }, right = { { 9, 9 } };
			Json::Value eyes;
			eyes["ret"] = sys->GetEyeTrackedFoveationCenter(&left, &right);
			eyes["left"] = ndc(left);
			eyes["right"] = ndc(right);
			c.Record("both eyes, gaze " + when, eyes);

			vr::HmdMatrix44_t own = sys->GetProjectionMatrix(vr::Eye_Left, 0.1f, 100.0f);
			vr::HmdMatrix44_t flipped = own;
			for (float& v : flipped.m[1])
				v = -v;
			vr::HmdMatrix44_t narrow = own;
			narrow.m[0][0] *= 2;
			narrow.m[1][1] *= 2;
			Json::Value projections;
			struct Named {
				const char* name;
				const vr::HmdMatrix44_t* m;
			};
			for (Named p : { Named{ "left", &own }, Named{ "flipped", &flipped }, Named{ "narrow", &narrow } }) {
				vr::HmdVector2_t centre = { { 9, 9 } };
				Json::Value r;
				r["ret"] = sys->GetEyeTrackedFoveationCenterForProjection(p.m, &centre);
				r["ndc"] = ndc(centre);
				projections[p.name] = r;
			}
			vr::HmdVector2_t unused = { { 9, 9 } };
			projections["null matrix"] = sys->GetEyeTrackedFoveationCenterForProjection(nullptr, &unused);
			c.Record("for projection, gaze " + when, projections);
		}
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}
	c.core->Cleanup();
}

// IVRInput's eye tracking action (expect/eye_tracking.json), on the stand-in with scripts/eye_gaze.json and
// --manifest manifest_eye_tracking.json: the gaze as the game sees it at frames 30 (tracked), 55 (not tracked) and
// 75 (not valid), in the standing and seated spaces, and the argument errors at frame 30. Values are rounded to 4
// places.
void EyeTrackingInputCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* in = (vr::IVRInput_011::IVRInput*)c.core->GetGenericInterface("IVRInput_011", &le);
	if (!in || c.manifest.empty()) {
		c.Record("no IVRInput_011 or manifest", Err(le));
		c.core->Cleanup();
		return;
	}
	in->SetActionManifestPath(c.manifest.c_str());
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t eyes = vr::k_ulInvalidActionHandle, trigger = vr::k_ulInvalidActionHandle;
	in->GetActionSetHandle("/actions/main", &set);
	in->GetActionHandle("/actions/main/in/eyes", &eyes);
	in->GetActionHandle("/actions/main/in/trigger", &trigger);

	auto round4 = [](float v) { return std::round((double)v * 10000) / 10000; };
	auto vec3 = [&](const vr::HmdVector3_t& v) {
		Json::Value a(Json::arrayValue);
		for (float x : v.v)
			a.append(round4(x));
		return a;
	};
	auto read = [&](bool nextFrame, vr::ETrackingUniverseOrigin origin) {
		vr::VREyeTrackingData_t data;
		memset(&data, 0xff, sizeof(data));
		Json::Value r;
		r["error"] = (int)(nextFrame ? in->GetEyeTrackingDataForNextFrame(eyes, origin, &data, sizeof(data))
		                             : in->GetEyeTrackingDataRelativeToNow(eyes, origin, -0.01f, &data, sizeof(data)));
		r["active"] = data.bActive;
		r["valid"] = data.bValid;
		r["tracked"] = data.bTracked;
		r["origin"] = vec3(data.vGazeOrigin);
		r["target"] = vec3(data.vGazeTarget);
		return r;
	};
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 75; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		vr::IVRInput_011::VRActiveActionSet_t active{};
		active.ulActionSet = set;
		in->UpdateActionState(&active, sizeof(active), 1);
		if (frame == 30 || frame == 55 || frame == 75) {
			std::string when = frame == 30 ? "tracked" : frame == 55 ? "not tracked" : "invalid";
			c.Record("for next frame, standing, gaze " + when, read(true, vr::TrackingUniverseStanding));
			c.Record("relative to now, seated, gaze " + when, read(false, vr::TrackingUniverseSeated));
		}
		if (frame == 30) {
			vr::VREyeTrackingData_t data;
			Json::Value errors;
			errors["null data"] = (int)in->GetEyeTrackingDataForNextFrame(eyes, vr::TrackingUniverseStanding, nullptr, sizeof(data));
			errors["wrong size"] = (int)in->GetEyeTrackingDataForNextFrame(eyes, vr::TrackingUniverseStanding, &data, sizeof(data) - 1);
			errors["invalid handle"] = (int)in->GetEyeTrackingDataForNextFrame(vr::k_ulInvalidActionHandle, vr::TrackingUniverseStanding, &data, sizeof(data));
			errors["wrong type"] = (int)in->GetEyeTrackingDataForNextFrame(trigger, vr::TrackingUniverseStanding, &data, sizeof(data));
			vr::IVRInput_011::InputDigitalActionData_t digital;
			errors["digital read"] = (int)in->GetDigitalActionData(eyes, &digital, sizeof(digital), vr::k_ulInvalidInputValueHandle);
			c.Record("errors", errors);
		}
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}
	c.core->Cleanup();
}

// Events between programs (expect/events.json): run at the same time (run.py --jobs 2), "events-commit"
// commits the room setup 1.5 s in, and "events-listen", polling for 5 s, gets its two events; the committing program
// gets each once, not again back from the store. Scene apps, each with its own session on the stand-in: OpenLamina
// gives a Utility app no IVRSystem, and the stand-in has no overlay or headless sessions by default
vr::IVRSystem_022::IVRSystem* OpenSceneSystem(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return nullptr;
	}
	vr::EVRInitError le;
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", &le);
	if (!sys) {
		c.Record("no IVRSystem_022", Err(le));
		c.core->Cleanup();
	}
	return sys;
}

Json::Value CountChaperoneEvents(vr::IVRSystem_022::IVRSystem* sys, int milliseconds)
{
	int data = 0, universe = 0;
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < until) {
		vr::VREvent_t ev{};
		while (sys->PollNextEvent(&ev, sizeof(ev))) {
			data += ev.eventType == vr::VREvent_ChaperoneDataHasChanged;
			universe += ev.eventType == vr::VREvent_ChaperoneUniverseHasChanged;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	Json::Value r;
	r["ChaperoneDataHasChanged"] = data;
	r["ChaperoneUniverseHasChanged"] = universe;
	return r;
}

void EventsListenCase(CaseContext& c)
{
	auto* sys = OpenSceneSystem(c);
	if (!sys)
		return;
	c.Record("events from the other program", CountChaperoneEvents(sys, 5000));
	c.core->Cleanup();
}

void EventsCommitCase(CaseContext& c)
{
	auto* sys = OpenSceneSystem(c);
	if (!sys)
		return;
	std::this_thread::sleep_for(std::chrono::milliseconds(1500));
	vr::EVRInitError le;
	auto* setup = (vr::IVRChaperoneSetup_006::IVRChaperoneSetup*)c.core->GetGenericInterface("IVRChaperoneSetup_006", &le);
	Json::Value commit;
	commit["ret"] = setup ? setup->CommitWorkingCopy(vr::IVRChaperoneSetup_006::EChaperoneConfigFile_Live) : false;
	c.Record("CommitWorkingCopy", commit);
	c.Record("own events", CountChaperoneEvents(sys, 1000));
	c.core->Cleanup();
}

// The session's changes as events (scripts/session_events.json): focus lost at frame 10 and back at 20, the IPD changed
// at 30, the headset taken off at 40, the session exiting at 50 (the session's frames). Each event of those kinds from
// frame 5 on, in order, and each kind's first one in full. With sessionEvents (expect/session_events.json) all of them;
// without (expect/session_events_off.json) the IPD's and the quit.
void SessionEventsCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	const std::set<uint32_t> kinds = { vr::VREvent_InputFocusCaptured, vr::VREvent_InputFocusReleased, vr::VREvent_IpdChanged,
		vr::VREvent_TrackedDeviceUserInteractionStarted, vr::VREvent_TrackedDeviceUserInteractionEnded, vr::VREvent_ButtonPress,
		vr::VREvent_ButtonUnpress, vr::VREvent_Quit };
	Json::Value sequence(Json::arrayValue);
	std::map<uint32_t, Json::Value> first;
	Json::Value proximity;
	auto worn = [&]() {
		vr::VRControllerState_t state{};
		bool ok = f.sys->GetControllerState(vr::k_unTrackedDeviceIndex_Hmd, &state, sizeof(state));
		return ok && (state.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_ProximitySensor)) != 0;
	};
	// The headset's state, read every frame: with XR_EXT_user_presence it comes from the runtime's presence events, not
	// from input, so the session isn't restarted for input and the script's frames are these
	for (int frame = 1; frame <= 70; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		const bool wornNow = worn();
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
		vr::VREvent_t ev{};
		for (int n = 0; n < 64 && f.sys->PollNextEvent(&ev, sizeof(ev)); n++) {
			if (frame < 5 || !kinds.count(ev.eventType))
				continue;
			sequence.append(ev.eventType);
			if (first.count(ev.eventType))
				continue;
			Json::Value e;
			e["frame"] = frame;
			e["device"] = ev.trackedDeviceIndex;
			if (ev.eventType == vr::VREvent_IpdChanged)
				e["ipd"] = Thousandths(ev.data.ipd.ipdMeters);
			if (ev.eventType == vr::VREvent_ButtonPress || ev.eventType == vr::VREvent_ButtonUnpress)
				e["button"] = ev.data.controller.button;
			first[ev.eventType] = e;
		}
		if (frame == 35)
			proximity["worn at 35"] = wornNow;
		if (frame == 47)
			proximity["worn at 47"] = wornNow;
	}
	proximity["sensor"] = f.sys->GetBoolTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_ContainsProximitySensor_Bool);
	Json::Value r;
	r["sequence"] = sequence;
	c.Record("events", r);
	for (auto& [type, e] : first)
		c.Record("event " + std::to_string(type), e);
	c.Record("proximity", proximity);
	c.core->Cleanup();
}

// A controller moving and turning (scripts/velocities.json: the right grip at (0.2, 1, -0.3), 1 m/s along +X, turning
// 2 rad/s about +Y): GetDeviceToAbsoluteTrackingPose gives both back. The pose is SteamVR's, offset from the grip, so its
// point moves by the turn as well: v at the grip is v - w x (pose - grip). (expect/velocities.json)
void VelocitiesCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 1; frame <= 10; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		// Reading the controllers is what starts legacy input
		for (vr::TrackedDeviceIndex_t i = 1; i < 16; i++) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(i, &state, sizeof(state));
		}
		f.comp->Submit(vr::Eye_Left, tex.get(), nullptr, vr::Submit_Default);
		f.comp->Submit(vr::Eye_Right, tex.get(), nullptr, vr::Submit_Default);
	}
	vr::TrackedDevicePose_t poses[16]{};
	f.sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, 16);
	const vr::TrackedDeviceIndex_t right = f.sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
	Json::Value r;
	r["found"] = right < 16;
	if (right < 16) {
		const vr::TrackedDevicePose_t& p = poses[right];
		r["valid"] = p.bPoseIsValid;
		r["v"] = ThousandthsOf(p.vVelocity);
		r["w"] = ThousandthsOf(p.vAngularVelocity);
		const float grip[3] = { 0.2f, 1.0f, -0.3f };
		const float d[3] = { p.mDeviceToAbsoluteTracking.m[0][3] - grip[0], p.mDeviceToAbsoluteTracking.m[1][3] - grip[1],
			p.mDeviceToAbsoluteTracking.m[2][3] - grip[2] };
		const float* w = p.vAngularVelocity.v;
		const vr::HmdVector3_t atGrip = { { p.vVelocity.v[0] - (w[1] * d[2] - w[2] * d[1]), p.vVelocity.v[1] - (w[2] * d[0] - w[0] * d[2]),
			p.vVelocity.v[2] - (w[0] * d[1] - w[1] * d[0]) } };
		r["v at grip"] = ThousandthsOf(atGrip);
	}
	// The head isn't scripted to move
	r["hmd v"] = ThousandthsOf(poses[vr::k_unTrackedDeviceIndex_Hmd].vVelocity);
	r["hmd w"] = ThousandthsOf(poses[vr::k_unTrackedDeviceIndex_Hmd].vAngularVelocity);
	c.Record("GetDeviceToAbsoluteTrackingPose", r);
	c.core->Cleanup();
}

void SubmitEdges(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	vr::TrackedDevicePose_t poses[16];
	f.comp->WaitGetPoses(poses, 16, nullptr, 0);
	c.Record("left", Err(f.comp->Submit(vr::Eye_Left, tex.get())));
	c.Record("left again", Err(f.comp->Submit(vr::Eye_Left, tex.get())));
	c.Record("right", Err(f.comp->Submit(vr::Eye_Right, tex.get())));

	f.comp->WaitGetPoses(poses, 16, nullptr, 0);
	vr::Texture_t nullTex{ nullptr, vr::TextureType_DirectX, vr::ColorSpace_Auto };
	c.Record("left null", Err(f.comp->Submit(vr::Eye_Left, &nullTex)));
	c.Record("right not null", Err(f.comp->Submit(vr::Eye_Right, tex.get())));

#ifdef _WIN32
	f.comp->WaitGetPoses(poses, 16, nullptr, 0);
	vr::Texture_t odd{ nullptr, vr::TextureType_DirectX, vr::ColorSpace_Auto };
	if (c.call->d3d11) {
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = 256;
		desc.MipLevels = desc.ArraySize = 1;
		desc.Format = (DXGI_FORMAT)78;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ID3D11Texture2D* t = nullptr;
		if (SUCCEEDED(c.call->d3d11->CreateTexture2D(&desc, nullptr, &t)))
			odd.handle = t;
	}
	Json::Value r = Err(odd.handle ? (int)f.comp->Submit(vr::Eye_Left, &odd) : -1);
	r["texture_made"] = odd.handle != nullptr;
	c.Record("left DXGI format 78", r);
	f.comp->Submit(vr::Eye_Right, tex.get());
	if (odd.handle)
		((ID3D11Texture2D*)odd.handle)->Release();
#endif

	vr::Texture_t six[6];
	for (vr::Texture_t& t : six)
		t = *tex.get();
	c.Record("SetSkyboxOverride six faces", Err(f.comp->SetSkyboxOverride(six, 6)));
	c.core->Cleanup();

	// A Background app submitting (refused at Init on Windows, allowed on Linux for Proton)
	vr::EVRInitError e = c.core->Init(vr::VRApplication_Background, nullptr);
	Json::Value bg;
	bg["init_error"] = (int)e;
	if (e == vr::VRInitError_None) {
		vr::EVRInitError le;
		auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", &le);
		bg["ret"] = comp ? (int)comp->Submit(vr::Eye_Left, tex.get()) : -1;
		c.core->Cleanup();
	}
	c.Record("Background submit", bg);
}

// One frame of the game's: poses, then both eyes
void GameFrame(Frames& f, const vr::Texture_t* tex)
{
	vr::TrackedDevicePose_t poses[16];
	f.comp->WaitGetPoses(poses, 16, nullptr, 0);
	f.comp->Submit(vr::Eye_Left, tex, nullptr, vr::Submit_Default);
	f.comp->Submit(vr::Eye_Right, tex, nullptr, vr::Submit_Default);
}

// An overlay shown from memory (64x64 grey), so OpenLamina has something of its own to show in a stall
bool ShowGreyOverlay(CaseContext& c)
{
	vr::EVRInitError le = vr::VRInitError_None;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	if (!ov || ov->CreateOverlay("oc.test.own", "Own", &h) != 0)
		return false;
	std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
	return ov->SetOverlayRaw(h, grey.data(), 64, 64, 4) == 0 && ov->ShowOverlay(h) == 0;
}

// The compositor's statistics and timing after 30 of the game's frames (expect/compositor.json)
void CompositorStatsCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 0; frame < 30; frame++)
		GameFrame(f, tex.get());

	Json::Value r;
	vr::IVRCompositor_029::Compositor_CumulativeStats stats{};
	f.comp->GetCumulativeStats(&stats, sizeof(stats));
	r["pid_set"] = stats.m_nPid != 0;
	r["presents_at_least_29"] = stats.m_nNumFramePresents >= 29;
	r["dropped"] = stats.m_nNumDroppedFrames;

	vr::Compositor_FrameTiming timings[5]{};
	timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
	uint32_t got = f.comp->GetFrameTimings(timings, 5);
	r["timings"] = got;
	r["timings_oldest_first"] = got == 5 && timings[0].m_nFrameIndex < timings[1].m_nFrameIndex;
	r["timing_sizes"] = got == 5 && timings[4].m_nSize == sizeof(vr::Compositor_FrameTiming);
	vr::Compositor_FrameTiming ago{};
	ago.m_nSize = sizeof(ago);
	r["frame_timing_3_ago"] = f.comp->GetFrameTiming(&ago, 3) && got == 5 && ago.m_nFrameIndex == timings[1].m_nFrameIndex;

	// Within a frame: never negative, and well under a second (the real clock, on a runner that may be busy)
	float left = f.comp->GetFrameTimeRemaining();
	r["time_remaining_in_a_frame"] = left >= 0.0f && left < 1.0f;

	uint32_t render = 0, game = 0;
	r["prediction_ids"] = (int)f.comp->GetLastPosePredictionIDs(&render, &game);
	r["prediction_ids_match"] = render != 0 && render == game;
	vr::TrackedDevicePose_t poses[16]{};
	r["poses_for_last_frame"] = (int)f.comp->GetPosesForFrame(render, poses, 16);
	r["poses_for_old_frame"] = (int)f.comp->GetPosesForFrame(render - 40, poses, 16);

	r["fullscreen"] = f.comp->IsFullscreen();
	r["motion_smoothing_supported"] = f.comp->IsMotionSmoothingSupported();
	r["motion_smoothing_enabled"] = f.comp->IsMotionSmoothingEnabled();
	// Frames the game waits for and submits nothing to aren't presents: the first wait ends the last submitted frame
	vr::IVRCompositor_029::Compositor_CumulativeStats before{}, after{};
	f.comp->GetCumulativeStats(&before, sizeof(before));
	for (int frame = 0; frame < 5; frame++) {
		vr::TrackedDevicePose_t waited[16];
		f.comp->WaitGetPoses(waited, 16, nullptr, 0);
	}
	f.comp->GetCumulativeStats(&after, sizeof(after));
	r["waits_without_submit_not_presents"] = after.m_nNumFramePresents - before.m_nNumFramePresents <= 1;

	f.comp->CompositorGoToBack();
	f.comp->CompositorQuit();
	c.core->Cleanup();
	c.Record("stats", r);
}

// OpenLamina's own frames while the game stalls (expect/own_frames.json, with ownFrames=true): with an overlay shown,
// 10 frames, a second without any call, then 10 more. With the stand-in at 90 Hz, OpenLamina runs at least 60 frames
// of its own in that second, each with the game's last frame again under the overlay, and no frame call comes out of
// order.
void OwnFramesCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	bool shown = ShowGreyOverlay(c);
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());
	std::this_thread::sleep_for(std::chrono::seconds(1));
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());

	Json::Value r;
#ifdef _WIN32
	// The game's device, which own frames' xrEndFrame shares with the game's threads, is multithread protected
	if (c.call->d3d11) {
		ID3D11DeviceContext* context = nullptr;
		c.call->d3d11->GetImmediateContext(&context);
		ID3D11Multithread* multithread = nullptr;
		if (context && SUCCEEDED(context->QueryInterface(__uuidof(ID3D11Multithread), (void**)&multithread)) && multithread) {
			r["device_multithread_protected"] = multithread->GetMultithreadProtected() != FALSE;
			multithread->Release();
		}
		if (context)
			context->Release();
	}
#endif
	c.core->Cleanup();

	r["frames"] = 20;
	r["overlay_shown"] = shown;
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["frames_ended_at_least_80"] = dump["frames_ended"].asInt64() >= 10 + 10 + 60;
		// Every frame recorded with a quad (the overlay) also has the game's frame (its first may be lost to the
		// session's move onto its device)
		std::map<int64_t, std::pair<bool, bool>> frames; // frame: quad, projection
		for (const Json::Value& layer : dump["layers"]) {
			auto& f = frames[layer["frame"].asInt64()];
			f.first |= layer["type"].asString() == "quad";
			f.second |= layer["type"].asString() == "projection";
		}
		int withQuad = 0, withBoth = 0;
		for (auto& [frame, f] : frames) {
			withQuad += f.first;
			withBoth += f.first && f.second;
		}
		r["own_frames_show_the_game_frame"] = withQuad >= 60 && withBoth >= withQuad - 1;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("frames", r);
}

// An overlay's image replaced from another thread, in alternating sizes, while OpenLamina's own frames draw it
// (expect/own_frames.json, with ownFrames=true): nothing crashes and no frame call comes out of order
void OwnFramesOverlayChurnCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	vr::EVRInitError le = vr::VRInitError_None;
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	if (!ov || ov->CreateOverlay("oc.test.churn", "Churn", &h) != 0) {
		c.Record("no overlay");
		c.core->Cleanup();
		return;
	}
	std::vector<uint8_t> grey(128 * 128 * 4, 0x80);
	ov->SetOverlayRaw(h, grey.data(), 64, 64, 4);
	ov->ShowOverlay(h);
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());
	std::atomic<int> sets{ 0 };
	std::thread setter([&] {
		// 60 images over about a second: the own frames run meanwhile
		for (int i = 0; i < 60; i++) {
			uint32_t size = i % 2 ? 128 : 64;
			if (ov->SetOverlayRaw(h, grey.data(), size, size, 4) == 0)
				sets++;
			std::this_thread::sleep_for(std::chrono::milliseconds(15));
		}
	});
	setter.join();
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());
	c.core->Cleanup();

	Json::Value r;
	r["sets"] = sets.load();
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["own_frames_ran"] = dump["frames_ended"].asInt64() > 20 + 30;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("frames", r);
}

// The same stall with nothing of OpenLamina's own to show (expect/own_frames.json): no frames of its own, so the
// runtime is left with the game's last frame
void OwnFramesNothingShownCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	for (int frame = 0; frame < 10; frame++)
		GameFrame(f, tex.get());
	c.core->Cleanup();

	Json::Value r;
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		// The game's 20, or one fewer (its first may be lost to the session's move onto its device)
		r["only_the_game_frames"] = dump["frames_ended"].asInt64() <= 20;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("frames", r);
}

// The game stalling and coming back again and again while another thread reads input (expect/own_frames.json): 20
// rounds of 5 frames and a 150 ms pause, each pause long enough for OpenLamina's own frames to start, so the game's
// next frame often meets one of them. No frame call may come out of order, and nothing may crash.
void OwnFramesRaceCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	ShowGreyOverlay(c);
	std::atomic<bool> done{ false };
	std::atomic<int> polls{ 0 };
	std::thread reader([&] {
		// Until the game's thread is done, but no more than 100000 passes
		for (int pass = 0; !done && pass < 100000; pass++) {
			for (vr::TrackedDeviceIndex_t dev = 1; dev <= 2; dev++) {
				vr::VRControllerState_t state{};
				f.sys->GetControllerState(dev, &state, sizeof(state));
			}
			polls++;
			std::this_thread::yield();
		}
	});
	const int rounds = 20, framesPerRound = 5;
	for (int round = 0; round < rounds; round++) {
		for (int frame = 0; frame < framesPerRound; frame++)
			GameFrame(f, tex.get());
		std::this_thread::sleep_for(std::chrono::milliseconds(150));
	}
	done = true;
	reader.join();
	c.core->Cleanup();

	Json::Value r;
	r["frames"] = rounds * framesPerRound;
	r["polled"] = polls > 0;
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["own_frames_ran"] = dump["frames_ended"].asInt64() > rounds * framesPerRound;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("frames", r);
}

// The quads in the stand-in's dump of a size (w x h metres, to the millimetre), counted over the frames it kept
int QuadsOfSize(const Json::Value& dump, double w, double h)
{
	int n = 0;
	for (const Json::Value& layer : dump["layers"]) {
		if (layer["type"].asString() != "quad" || layer["size"].size() != 2)
			continue;
		if (Milli(layer["size"][0].asDouble()) == Milli(w) && Milli(layer["size"][1].asDouble()) == Milli(h))
			n++;
	}
	return n;
}

// The overlay events' names, in order, a repeated one once
std::string EventName(uint32_t type)
{
	switch (type) {
	case vr::VREvent_FocusEnter:
		return "enter";
	case vr::VREvent_FocusLeave:
		return "leave";
	case vr::VREvent_MouseMove:
		return "move";
	case vr::VREvent_MouseButtonDown:
		return "down";
	case vr::VREvent_MouseButtonUp:
		return "up";
	case vr::VREvent_ScrollDiscrete:
		return "scroll";
	case vr::VREvent_ScrollSmooth:
		return "scroll-smooth";
	case vr::VREvent_OverlayShown:
		return "shown";
	case vr::VREvent_OverlayHidden:
		return "hidden";
	default:
		return std::to_string(type);
	}
}

// The right controller's laser on an overlay that takes mouse input (expect/laser_mouse.json, with
// scripts/laser.json): a 1 m overlay at (0, 1.5, -2), 200 x 100 in mouse units; the right aim pose at (0, 1.5, -1)
// looking along -Z from frame 5, its trigger held from frame 10 to 12. The game reads the old input from frame 2,
// which loads the actions the lasers read. PollNextOverlayEvent gives OverlayShown (from ShowOverlay), then
// FocusEnter, MouseMove, MouseButtonDown and Up, all at the overlay's centre, and OverlayHidden once it's hidden; the
// stand-in is sent a 2 cm cursor quad.
void LaserMouseCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* ov = (IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!ov) {
		c.Record("no IVROverlay_028", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
	Json::Value set;
	set["create"] = (int)ov->CreateOverlay("oc.laser", "Laser", &h);
	std::vector<uint8_t> grey(64 * 64 * 4, 0x80);
	set["raw"] = (int)ov->SetOverlayRaw(h, grey.data(), 64, 64, 4);
	set["width"] = (int)ov->SetOverlayWidthInMeters(h, 1);
	vr::HmdMatrix34_t m = Translation(0, 1.5f, -2);
	set["transform"] = (int)ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
	set["input_method"] = (int)ov->SetOverlayInputMethod(h, VROverlayInputMethod_Mouse);
	vr::HmdVector2_t scale{ { 200, 100 } };
	set["mouse_scale"] = (int)ov->SetOverlayMouseScale(h, &scale);
	set["show"] = (int)ov->ShowOverlay(h);
	c.Record("set", set);

	Json::Value order(Json::arrayValue);
	std::string last;
	Json::Value down, up;
	bool hovered = false;
	bool entered = false;
	int haptic = -1;
	for (int frame = 1; frame <= 30; frame++) {
		GameFrame(f, tex.get());
		if (frame >= 2) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(2, &state, sizeof(state));
		}
		vr::VREvent_t ev{};
		while (ov->PollNextOverlayEvent(h, &ev, sizeof(ev))) {
			const std::string name = EventName(ev.eventType);
			if (name != last)
				order.append(name);
			last = name;
			entered = entered || ev.eventType == vr::VREvent_FocusEnter;
			Json::Value* at = ev.eventType == vr::VREvent_MouseButtonDown ? &down : (ev.eventType == vr::VREvent_MouseButtonUp ? &up : nullptr);
			if (at && at->isNull()) {
				(*at)["x"] = Milli(ev.data.mouse.x);
				(*at)["y"] = Milli(ev.data.mouse.y);
				(*at)["button"] = ev.data.mouse.button;
				(*at)["device"] = (int)ev.trackedDeviceIndex;
			}
		}
		if (entered && !hovered) {
			hovered = ov->IsHoverTargetOverlay(h);
			haptic = (int)ov->TriggerLaserMouseHapticVibration(h, 0.05f, 0, 1);
		}
	}
	// Hidden: told once, and not again for an overlay that's hidden already
	ov->HideOverlay(h);
	ov->HideOverlay(h);
	vr::VREvent_t hiddenEvent{};
	while (ov->PollNextOverlayEvent(h, &hiddenEvent, sizeof(hiddenEvent)))
		order.append(EventName(hiddenEvent.eventType));
	ov->DestroyOverlay(h);
	c.core->Cleanup();

	Json::Value r;
	r["order"] = order;
	r["down"] = down;
	r["up"] = up;
	r["hover_target"] = hovered;
	r["haptic"] = haptic;
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["cursor_quad"] = QuadsOfSize(dump, 0.02, 0.02) > 0;
		r["frame_order_errors"] = dump["frame_order_errors"];
		// No frame refused: the cursor's swapchain has its image before a layer shows it
		r["frames_refused"] = (Json::UInt64)dump["frames_refused"].asUInt64();
	}
	c.Record("events", r);
}

// A message box answered with the laser (expect/message_overlay.json, with scripts/message_box.json): after 5 frames
// (reading the old input from frame 2), ShowMessageOverlay with two buttons. Its panel (1 m x 0.5 m) is 1.5 m ahead of
// the head, at (0, 1.7, -1.5); the right aim pose is on the second button from the start, and the script's trigger
// clicks it at frame 20. The call returns ButtonPress_1, after frames of OpenLamina's own, in order. Another thread
// closes the box after 10 s if it's still up, so the case always ends; it's joined before the case does.
void MessageOverlayCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* ov = (IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!ov) {
		c.Record("no IVROverlay_028", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	const int gameFrames = 10;
	for (int frame = 1; frame <= gameFrames / 2; frame++) {
		GameFrame(f, tex.get());
		if (frame >= 2) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(2, &state, sizeof(state));
		}
	}
	std::atomic<bool> answered{ false };
	std::atomic<bool> closedByWatchdog{ false };
	std::thread watchdog([&] {
		// At most 10 s, in 50 ms steps
		for (int i = 0; i < 200 && !answered; i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		if (!answered) {
			closedByWatchdog = true;
			ov->CloseMessageOverlay();
		}
	});
	const int ret = (int)ov->ShowMessageOverlay("Save the game before quitting?", "OpenLamina test", "Cancel", "Save");
	answered = true;
	watchdog.join();
	for (int frame = 0; frame < gameFrames / 2; frame++)
		GameFrame(f, tex.get());
	c.core->Cleanup();

	Json::Value r;
	r["ret"] = ret;
	r["closed_by_watchdog"] = closedByWatchdog.load();
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["own_frames_ran"] = dump["frames_ended"].asInt64() > gameFrames;
		r["panel_quad"] = QuadsOfSize(dump, 1.0, 0.5) > 0;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("ShowMessageOverlay", r);
}

// The keyboard typed on with the laser (expect/keyboard.json, with scripts/keyboard.json and ini/keyboard_us.ini):
// ShowKeyboard at frame 4 (normal, one line); the keyboard (the US layout, 1 m x 0.404 m) is 1 m ahead of the head and
// 0.4 m below it, at (0, 1.3, -1). The script points the right laser at Shift, H, I and Done in turn, clicking each:
// two KeyboardCharInput events ("H" and "i"), then KeyboardDone, and GetKeyboardText answers "Hi". The stand-in is
// sent the keyboard's quad while it's shown.
void KeyboardCase(CaseContext& c)
{
	using namespace vr::IVROverlay_028;
	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* ov = (IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!ov) {
		c.Record("no IVROverlay_028", Err(le));
		c.core->Cleanup();
		return;
	}
	callers::Texture tex(ArgSet::Valid, *c.call);
	int shown = -1;
	Json::Value chars(Json::arrayValue);
	int done = 0, closed = 0;
	uint64_t userValue = 0;
	for (int frame = 1; frame <= 80; frame++) {
		GameFrame(f, tex.get());
		if (frame >= 2) {
			vr::VRControllerState_t state{};
			f.sys->GetControllerState(2, &state, sizeof(state));
		}
		if (frame == 4)
			shown = (int)ov->ShowKeyboard(k_EGamepadTextInputModeNormal, k_EGamepadTextInputLineModeSingleLine, 0, "Name", 0, "", 42);
		vr::VREvent_t ev{};
		while (f.sys->PollNextEvent(&ev, sizeof(ev))) {
			if (ev.eventType == vr::VREvent_KeyboardCharInput) {
				char text[9] = {};
				memcpy(text, ev.data.keyboard.cNewInput, 8);
				chars.append(text);
				userValue = ev.data.keyboard.uUserValue;
			} else if (ev.eventType == vr::VREvent_KeyboardDone) {
				done++;
			} else if (ev.eventType == vr::VREvent_KeyboardClosed) {
				closed++;
			}
		}
	}
	char text[64] = {};
	const uint32_t length = ov->GetKeyboardText(text, sizeof(text));
	c.core->Cleanup();

	Json::Value r;
	r["show"] = shown;
	r["chars"] = chars;
	r["user_value"] = (Json::UInt64)userValue;
	r["done"] = done;
	r["closed"] = closed;
	r["text"] = text;
	r["text_length"] = length;
	Json::Value dump = ReadStandInDump();
	if (!dump.isNull()) {
		r["keyboard_quad"] = QuadsOfSize(dump, 1.0, 0.404) > 0;
		r["frame_order_errors"] = dump["frame_order_errors"];
	}
	c.Record("typed", r);
}

#ifdef _WIN32
// The DXGI adapters (DXGI 1.1's list, which DXGI 1.0's EnumAdapters has in the same order): LUIDs as LowPart |
// HighPart << 32
std::vector<uint64_t> DxgiAdapterLuids()
{
	std::vector<uint64_t> luids;
	IDXGIFactory1* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)))
		return luids;
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; i < 64 && factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 desc{};
		adapter->GetDesc1(&desc);
		adapter->Release();
		luids.push_back((uint64_t)desc.AdapterLuid.LowPart | ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32));
	}
	factory->Release();
	return luids;
}

// Has the stand-in name this adapter in its D3D requirements (read when OpenLamina asks, so set before Init)
void NameRuntimeAdapter(uint64_t luid)
{
	char hex[32];
	std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)luid);
	SetEnvironmentVariableA("OC_TEST_RUNTIME_GPU_LUID", hex);
}

// D3D9Ex's index of the adapter with luid, 0 if it has none (as OpenLamina answers then)
int32_t D3D9AdapterIndexOf(uint64_t luid)
{
	HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
	if (!d3d9)
		return 0;
	using Create9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
	auto create = (Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");
	IDirect3D9Ex* d3d = nullptr;
	int32_t index = 0;
	if (create && SUCCEEDED(create(D3D_SDK_VERSION, &d3d)) && d3d) {
		for (UINT i = 0; i < d3d->GetAdapterCount() && i < 64; i++) {
			LUID l{};
			if (SUCCEEDED(d3d->GetAdapterLUID(i, &l)) && ((uint64_t)l.LowPart | ((uint64_t)(uint32_t)l.HighPart << 32)) == luid) {
				index = (int32_t)i;
				break;
			}
		}
		d3d->Release();
	}
	FreeLibrary(d3d9);
	return index;
}

// The last physical device of instance, with its deviceUUID named to the stand-in as the runtime's
// (OC_TEST_RUNTIME_GPU_UUID); VK_NULL_HANDLE without one
VkPhysicalDevice NameRuntimeVkDevice(VkInstance instance)
{
	HMODULE module = GetModuleHandleA("vulkan-1.dll");
	auto gipa = module ? (PFN_vkGetInstanceProcAddr)GetProcAddress(module, "vkGetInstanceProcAddr") : nullptr;
	if (!gipa || !instance)
		return VK_NULL_HANDLE;
	auto enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
	auto props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
	if (!props2)
		props2 = (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2KHR");
	uint32_t count = 0;
	if (!enumerate || !props2 || enumerate(instance, &count, nullptr) != VK_SUCCESS || count == 0)
		return VK_NULL_HANDLE;
	std::vector<VkPhysicalDevice> devices(count);
	if (enumerate(instance, &count, devices.data()) < 0 || count == 0)
		return VK_NULL_HANDLE;
	VkPhysicalDevice last = devices[count - 1];
	VkPhysicalDeviceIDProperties id{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 p{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	p.pNext = &id;
	props2(last, &p);
	std::string hex;
	char buf[3];
	for (uint8_t b : id.deviceUUID) {
		std::snprintf(buf, sizeof(buf), "%02x", b);
		hex += buf;
	}
	SetEnvironmentVariableA("OC_TEST_RUNTIME_GPU_UUID", hex.c_str());
	return last;
}
#endif

// The runtime's GPU as OpenLamina answers it (expect/gpu.json), on the stand-in with scripts/gfx_all_apis.json: the
// stand-in's D3D requirements name the last DXGI adapter (WARP's, where there's a GPU as well) and its Vulkan the last
// physical device. Each answer is recorded as whether it's that adapter's; the Vulkan one (a record of its own) is
// skipped without a Vulkan driver.
void GpuAnswersCase(CaseContext& c)
{
#ifdef _WIN32
	std::vector<uint64_t> luids = DxgiAdapterLuids();
	if (luids.empty()) {
		Json::Value r;
		r["skipped"] = "no DXGI adapter";
		c.Record("answers", r);
		return;
	}
	const uint64_t runtimeLuid = luids.back();
	const int32_t runtimeIndex = (int32_t)luids.size() - 1;
	NameRuntimeAdapter(runtimeLuid);

	std::string whyNoVulkan;
	std::unique_ptr<GfxApi> vk = GfxApi::Create(GfxKind::Vulkan, &whyNoVulkan);
	GfxTexture vkTex = vk ? vk->Pattern(16, 16, 43, 0) : GfxTexture{}; // VK_FORMAT_R8G8B8A8_SRGB
	VkInstance vkInstance = vkTex.Made() ? (VkInstance)vkTex.vk.m_pInstance : VK_NULL_HANDLE;
	VkPhysicalDevice runtimeVk = NameRuntimeVkDevice(vkInstance);

	Frames f;
	if (!OpenFrames(c, f))
		return;
	vr::EVRInitError le;
	auto* display = (vr::IVRExtendedDisplay_001::IVRExtendedDisplay*)c.core->GetGenericInterface("IVRExtendedDisplay_001", &le);

	Json::Value r;
	int32_t dxgi = -1;
	f.sys->GetDXGIOutputInfo(&dxgi);
	r["dxgi_is_runtime_adapter"] = dxgi == runtimeIndex;
	r["d3d9_is_runtime_adapter"] = f.sys->GetD3D9AdapterIndex() == D3D9AdapterIndexOf(runtimeLuid);
	if (display) {
		int32_t adapter = -1, output = -1;
		display->GetDXGIOutputInfo(&adapter, &output);
		r["extended_is_runtime_adapter"] = adapter == runtimeIndex;
		r["extended_output"] = output;
	}
	uint64_t device = 0;
	f.sys->GetOutputDevice(&device, vr::TextureType_DirectX, nullptr);
	r["output_device_d3d11_is_runtime_luid"] = device == runtimeLuid;
	device = 0;
	f.sys->GetOutputDevice(&device, vr::TextureType_DirectX12, nullptr);
	r["output_device_d3d12_is_runtime_luid"] = device == runtimeLuid;
	c.Record("answers", r);

	Json::Value v;
	if (!runtimeVk) {
		v["skipped"] = vk ? "no Vulkan physical device" : whyNoVulkan;
	} else {
		device = 0;
		f.sys->GetOutputDevice(&device, vr::TextureType_Vulkan, (VkInstance_T*)vkInstance);
		v["output_device_is_runtime_device"] = device == (uint64_t)runtimeVk;
	}
	c.Record("vulkan", v);
	c.core->Cleanup();
#else
	Json::Value r;
	r["skipped"] = "the GPU cases are Windows only";
	c.Record("answers", r);
#endif
}

// A game whose device is on another adapter than the runtime's (expect/gpu.json): the stand-in names the last DXGI
// adapter, the game's D3D11 device is on the first. Submit answers SharedTexturesNotSupported (106) for both eyes.
// Skipped with one adapter.
void GpuOtherAdapterCase(CaseContext& c)
{
#ifdef _WIN32
	std::vector<uint64_t> luids = DxgiAdapterLuids();
	std::string why;
	std::unique_ptr<GfxApi> gfx = luids.size() >= 2 ? GfxApi::Create(GfxKind::D3D11, &why) : nullptr;
	if (!gfx) {
		Json::Value r;
		r["skipped"] = luids.size() < 2 ? "only one DXGI adapter" : why;
		c.Record("submit", r);
		return;
	}
	GfxTexture tex = gfx->Pattern(256, 256, 29, 0); // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	ID3D11Device* device = nullptr;
	if (tex.Made())
		((ID3D11Texture2D*)tex.keep)->GetDevice(&device);
	uint64_t gameLuid = 0;
	if (device) {
		IDXGIDevice* dxgi = nullptr;
		IDXGIAdapter* adapter = nullptr;
		DXGI_ADAPTER_DESC desc{};
		if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter))
		    && SUCCEEDED(adapter->GetDesc(&desc)))
			gameLuid = (uint64_t)desc.AdapterLuid.LowPart | ((uint64_t)(uint32_t)desc.AdapterLuid.HighPart << 32);
		if (adapter)
			adapter->Release();
		if (dxgi)
			dxgi->Release();
		device->Release();
	}
	// The runtime on another adapter than the game's
	uint64_t runtimeLuid = luids.back() != gameLuid ? luids.back() : luids.front();
	NameRuntimeAdapter(runtimeLuid);
	Frames f;
	if (!OpenFrames(c, f))
		return;
	Json::Value r;
	for (int frame = 0; frame < 3; frame++) {
		vr::TrackedDevicePose_t poses[16];
		f.comp->WaitGetPoses(poses, 16, nullptr, 0);
		r["left"] = (int)f.comp->Submit(vr::Eye_Left, tex.Texture(), nullptr, vr::Submit_Default);
		r["right"] = (int)f.comp->Submit(vr::Eye_Right, tex.Texture(), nullptr, vr::Submit_Default);
	}
	c.core->Cleanup();
	c.Record("submit", r);
#else
	Json::Value r;
	r["skipped"] = "the GPU cases are Windows only";
	c.Record("submit", r);
#endif
}

// Prop_DisplayFrequency_Float over 20 frames (expect/display_frequency_*.json): before the first frame, at frame 5 and
// at the end, rounded to whole hertz, and the first frame whose events had a VREvent_PropertyChanged for it (-1 for
// none, recorded as is and as whether it's the 10th or 11th). With scripts/refresh_72_120.json the runtime offers XR_FB_display_refresh_rate and changes from 72 to 120 Hz
// at its 10th frame; with scripts/display_120.json it has no such extension and a 120 Hz display.
void DisplayFrequencyCase(CaseContext& c)
{
	Frames f;
	if (!OpenFrames(c, f))
		return;
	callers::Texture tex(ArgSet::Valid, *c.call);
	auto frequency = [&] {
		vr::ETrackedPropertyError e = vr::TrackedProp_Success;
		return (int)std::lround(f.sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &e));
	};
	Json::Value r;
	r["before_frames"] = frequency();
	int changedAt = -1;
	for (int frame = 1; frame <= 20; frame++) {
		GameFrame(f, tex.get());
		vr::VREvent_t ev{};
		for (int n = 0; n < 64 && f.sys->PollNextEvent(&ev, sizeof(ev)); n++) {
			if (changedAt < 0 && ev.eventType == vr::VREvent_PropertyChanged && ev.trackedDeviceIndex == vr::k_unTrackedDeviceIndex_Hmd
			    && ev.data.property.prop == vr::Prop_DisplayFrequency_Float)
				changedAt = frame;
		}
		if (frame == 5)
			r["at_frame_5"] = frequency();
	}
	r["at_frame_20"] = frequency();
	r["changed_at_frame"] = changedAt;
	// The runtime's 10th frame is the game's 10th or 11th (its first may be on the session before the game's device)
	r["changed_at_frame_10_or_11"] = changedAt == 10 || changedAt == 11;
	c.Record("frequency", r);
	c.core->Cleanup();
}

// A 3x4 matrix as rows of numbers to 4 decimals (no -0)
Json::Value Matrix34Record(const vr::HmdMatrix34_t& m)
{
	Json::Value rows(Json::arrayValue);
	for (int r = 0; r < 3; r++) {
		Json::Value row(Json::arrayValue);
		for (int col = 0; col < 4; col++) {
			double v = std::round((double)m.m[r][col] * 10000) / 10000;
			row.append(v == 0 ? 0.0 : v);
		}
		rows.append(row);
	}
	return rows;
}

// SteamVR's render models from its install (expect/render_models.json). run.py points OC_TEST_STEAMVR_DIR at
// tests/unit/data/steamvr, a fixture of our own: test_controller (a body and a trigger, one PNG) and vive_tracker,
// whose JSON isn't JSON, so the built-in tracker is answered.
void RenderModelsCase(CaseContext& c)
{
	using namespace vr::IVRRenderModels_006;
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le;
	auto* rm = (IVRRenderModels*)c.core->GetGenericInterface("IVRRenderModels_006", &le);
	if (!rm) {
		c.Record("no IVRRenderModels_006", Err(le));
		c.core->Cleanup();
		return;
	}
	// A model or texture is answered at once or within 100 tries, 10 ms apart
	auto untilLoaded = [](auto call) {
		EVRRenderModelError e = VRRenderModelError_Loading;
		for (int i = 0; i < 100 && e == VRRenderModelError_Loading; i++) {
			e = call();
			if (e == VRRenderModelError_Loading)
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return e;
	};
	auto load = [&](const char* name, RenderModel_t** out) {
		return (int)untilLoaded([&] { return rm->LoadRenderModel_Async(name, out); });
	};

	Json::Value r;
	uint32_t count = rm->GetRenderModelCount();
	r["count"] = count;
	Json::Value names(Json::arrayValue);
	for (uint32_t i = 0; i < count && i < 64; i++) {
		char name[256] = {};
		rm->GetRenderModelName(i, name, sizeof(name));
		names.append(name);
	}
	r["names"] = names;
	c.Record("models", r);

	r = Json::Value();
	RenderModel_t* model = nullptr;
	r["ret"] = load("test_controller", &model);
	if (model) {
		r["vertices"] = model->unVertexCount;
		r["triangles"] = model->unTriangleCount;
		bool inRange = true;
		for (uint32_t i = 0; i < model->unTriangleCount * 3; i++)
			inRange = inRange && model->rIndexData[i] < model->unVertexCount;
		r["indices_in_range"] = inRange;
		r["textured"] = model->diffuseTextureId >= 0;
		RenderModel_TextureMap_t* tex = nullptr;
		TextureID_t id = model->diffuseTextureId;
		r["texture_ret"] = (int)untilLoaded([&] { return rm->LoadTexture_Async(id, &tex); });
		if (tex) {
			r["texture"] = std::to_string(tex->unWidth) + "x" + std::to_string(tex->unHeight);
			char px[16];
			std::snprintf(px, sizeof(px), "%02x%02x%02x%02x", tex->rubTextureMapData[0], tex->rubTextureMapData[1],
			    tex->rubTextureMapData[2], tex->rubTextureMapData[3]);
			r["first_pixel"] = px;
			rm->FreeTexture(tex);
		}
#ifdef _WIN32
		// The same texture made on the game's D3D11 device
		if (c.call->d3d11) {
			void* d3dTex = nullptr;
			r["d3d11_ret"] = (int)untilLoaded([&] { return rm->LoadTextureD3D11_Async(id, c.call->d3d11, &d3dTex); });
			if (d3dTex) {
				D3D11_TEXTURE2D_DESC desc{};
				((ID3D11Texture2D*)d3dTex)->GetDesc(&desc);
				r["d3d11_texture"] = std::to_string(desc.Width) + "x" + std::to_string(desc.Height);
				rm->FreeTextureD3D11(d3dTex);
			}
		}
#endif
		rm->FreeRenderModel(model);
	}
	c.Record("load test_controller", r);

	r = Json::Value();
	uint32_t components = rm->GetComponentCount("test_controller");
	r["count"] = components;
	Json::Value list(Json::arrayValue);
	for (uint32_t i = 0; i < components && i < 16; i++) {
		Json::Value one;
		char name[128] = {}, modelName[256] = {};
		one["name_size"] = rm->GetComponentName("test_controller", i, name, sizeof(name));
		one["name"] = name;
		rm->GetComponentRenderModelName("test_controller", name, modelName, sizeof(modelName));
		one["render_model"] = modelName;
		one["has"] = rm->RenderModelHasComponent("test_controller", name);
		one["buttons"] = (Json::UInt64)rm->GetComponentButtonMask("test_controller", name);
		list.append(one);
	}
	r["components"] = list;
	r["has_tip"] = rm->RenderModelHasComponent("test_controller", "tip");
	c.Record("components", r);

	r = Json::Value();
	model = nullptr;
	r["ret"] = load("test_controller/trigger", &model);
	if (model) {
		r["vertices"] = model->unVertexCount;
		r["triangles"] = model->unTriangleCount;
		rm->FreeRenderModel(model);
	}
	c.Record("load component", r);

	r = Json::Value();
	vr::VRControllerState_t state{};
	RenderModel_ControllerMode_State_t mode{};
	vr::RenderModel_ComponentState_t cs{};
	state.rAxis[1].x = 1.0f; // the trigger pulled
	r["ret"] = rm->GetComponentState("test_controller", "trigger", &state, &mode, &cs);
	r["render_model"] = Matrix34Record(cs.mTrackingToComponentRenderModel);
	r["local"] = Matrix34Record(cs.mTrackingToComponentLocal);
	r["properties"] = cs.uProperties;
	state.rAxis[1].x = 0.0f;
	rm->GetComponentState("test_controller", "trigger", &state, &mode, &cs);
	r["at_rest"] = Matrix34Record(cs.mTrackingToComponentRenderModel);
	rm->GetComponentState("test_controller", "body", &state, &mode, &cs);
	r["body_local"] = Matrix34Record(cs.mTrackingToComponentLocal);
	r["body_properties"] = cs.uProperties;
	c.Record("component state", r);

	r = Json::Value();
	char path[1024] = {};
	EVRRenderModelError pe = VRRenderModelError_Loading;
	uint32_t thumbSize = rm->GetRenderModelThumbnailURL("test_controller", path, sizeof(path), &pe);
	r["thumbnail_error"] = (int)pe;
	std::string url = path;
	const std::string png = "test_controller.png";
	r["thumbnail_is_its_png"] = url.rfind("file:///", 0) == 0 && url.size() > png.size() && url[8] != '/'
	    && url.compare(url.size() - png.size(), png.size(), png) == 0;
	r["thumbnail_size_matches"] = thumbSize == url.size() + 1;
	pe = VRRenderModelError_Loading;
	uint32_t originalSize = rm->GetRenderModelOriginalPath("test_controller", path, sizeof(path), &pe);
	r["original_error"] = (int)pe;
	std::string original = path;
	std::replace(original.begin(), original.end(), '\\', '/');
	const std::string obj = "rendermodels/test_controller/test_controller.obj";
	r["original_is_its_obj"] = original.size() > obj.size() && original.compare(original.size() - obj.size(), obj.size(), obj) == 0;
	r["original_size_matches"] = originalSize == std::string(path).size() + 1;
	c.Record("paths", r);

	r = Json::Value();
	model = nullptr;
	r["broken_ret"] = load("vive_tracker", &model);
	if (model) {
		r["broken_vertices"] = model->unVertexCount;
		r["broken_textured"] = model->diffuseTextureId >= 0;
		rm->FreeRenderModel(model);
	}
	model = nullptr;
	r["hand_ret"] = load("renderLeftHand", &model);
	if (model) {
		r["hand_has_vertices"] = model->unVertexCount > 0;
		rm->FreeRenderModel(model);
	}
	model = nullptr;
	r["unknown_ret"] = load("no_such_model", &model);
	r["unknown_null"] = model == nullptr;
	model = nullptr;
	r["outside_ret"] = load("../../bin", &model);
	r["outside_null"] = model == nullptr;
	c.Record("fallback", r);

	c.core->Cleanup();
}

} // namespace

const std::map<std::string, CaseFn>& conformance::Cases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "init-matrix", InitMatrix },
		{ "init-repeat", InitRepeat },
		{ "init-other-thread", InitOtherThread },
		{ "lookup-before-init", LookupBeforeInit },
		{ "unknown-interface", UnknownInterface },
		{ "overlay-lifecycle", OverlayLifecycle },
		{ "input-manifest", InputManifest },
		{ "frame-loop", FrameLoop },
		{ "old-events", OldEvents },
		{ "old-calls", OldCalls },
		{ "old-submit", OldSubmit },
		{ "old-overlay", OldOverlay },
		{ "overlay-transforms", OverlayTransformsCase },
		{ "old-misc", OldMisc },
		{ "new-interfaces", NewInterfaces },
		{ "device-identity", DeviceIdentityCase },
		{ "applications", ApplicationsCase },
		{ "chaperone", ChaperoneCase },
		{ "chaperone-read", ChaperoneReadCase },
		{ "chaperone-offsets", ChaperoneOffsetsCase },
		{ "events", EventsCase },
		{ "recentre", RecentreCase },
		{ "input-frames", InputFramesCase },
		{ "binding-info", BindingInfoCase },
		{ "eye-gaze", EyeGazeCase },
		{ "eye-tracking", EyeTrackingInputCase },
		{ "input-threads", InputThreadsCase },
		{ "events-listen", EventsListenCase },
		{ "events-commit", EventsCommitCase },
		{ "session-events", SessionEventsCase },
		{ "velocities", VelocitiesCase },
		{ "submit-edges", SubmitEdges },
		{ "observer", ObserverCase },
		{ "own-frames", OwnFramesCase },
		{ "own-frames-race", OwnFramesRaceCase },
		{ "own-frames-nothing-shown", OwnFramesNothingShownCase },
		{ "own-frames-overlay-churn", OwnFramesOverlayChurnCase },
		{ "laser-mouse", LaserMouseCase },
		{ "message-overlay", MessageOverlayCase },
		{ "keyboard", KeyboardCase },
		{ "compositor-stats", CompositorStatsCase },
		{ "gpu-answers", GpuAnswersCase },
		{ "gpu-other-adapter", GpuOtherAdapterCase },
		{ "display-frequency", DisplayFrequencyCase },
		{ "render-models", RenderModelsCase },
	};
	static const std::map<std::string, CaseFn> all = [] {
		std::map<std::string, CaseFn> m = cases;
		m.insert(GfxCases().begin(), GfxCases().end());
		m.insert(HelperCases().begin(), HelperCases().end());
		return m;
	}();
	return all;
}
