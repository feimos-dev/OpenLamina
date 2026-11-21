// Cases whose answers wait on a SteamVR recording with a headset and controllers on: run them with controllers on
// (scripts/controllers_on.json on the stand-in; switched on with SteamVR). Each writes one record per answer, as
// cases.cpp's do, and needs nothing that isn't there to stay ok without a runtime.

#include "conformance.h"
#include "gfx/GfxApi.h"

#include "IVRApplications_007.h"
#include "IVRChaperoneSetup_006.h"
#include "IVRChaperone_004.h"
#include "IVRCompositor_029.h"
#include "IVRIOBuffer_002.h"
#include "IVRInput_010.h"
#include "IVROverlay_028.h"
#include "IVRRenderModels_006.h"
#include "IVRSettings_003.h"
#include "IVRSystem_022.h"

#include <chrono>
#include <memory>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

using namespace conformance;

namespace {

Json::Value Ret(int e)
{
	Json::Value v;
	v["ret"] = e;
	return v;
}

double Milli(double v)
{
	double r = std::round(v * 1000) / 1000;
	return r == 0 ? 0.0 : r;
}

template <typename T>
T* Get(CaseContext& c, const char* version)
{
	vr::EVRInitError e = vr::VRInitError_None;
	auto* p = (T*)c.core->GetGenericInterface(version, &e);
	if (!p)
		c.Record(std::string("no ") + version, Ret(e));
	return p;
}

bool InitScene(CaseContext& c)
{
	vr::EVRInitError e = c.core->Init(vr::VRApplication_Scene, nullptr);
	if (e != vr::VRInitError_None) {
		c.Record("Init", Ret(e));
		return false;
	}
	return true;
}

// A few frames of a game's: poses, then both eyes submitted (the platform's own API: D3D11 on Windows, Vulkan
// elsewhere), and the events, so the session has started and the controllers are seen
void Frames(CaseContext& c, int n)
{
	static std::unique_ptr<GfxApi> gfx;
	static GfxTexture eyes[2];
	if (!gfx) {
		std::string why;
#ifdef _WIN32
		gfx = GfxApi::Create(GfxKind::D3D11, &why);
		const int64_t format = 29; // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
#else
		gfx = GfxApi::Create(GfxKind::Vulkan, &why);
		const int64_t format = 43; // VK_FORMAT_R8G8B8A8_SRGB
#endif
		if (gfx) {
			for (int eye = 0; eye < 2; eye++)
				eyes[eye] = gfx->Pattern(256, 256, format, eye);
			gfx->Flush();
		}
	}
	auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", nullptr);
	auto* sys = (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", nullptr);
	for (int i = 0; i < n; i++) {
		if (comp) {
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
			comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);
			for (int eye = 0; gfx && eye < 2; eye++) {
				if (eyes[eye].Made())
					comp->Submit((vr::EVREye)eye, eyes[eye].Texture());
			}
		} else {
			std::this_thread::sleep_for(std::chrono::milliseconds(11));
		}
		vr::VREvent_t ev;
		for (int k = 0; sys && k < 64 && sys->PollNextEvent(&ev, sizeof(ev)); k++) {
		}
	}
}

// Every connected device's identity properties and role, trackers included
void DevicePropertiesCase(CaseContext& c)
{
	if (!InitScene(c))
		return;
	auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	if (sys) {
		Frames(c, 10);
		for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
			if (!sys->IsTrackedDeviceConnected(i))
				continue;
			Json::Value d;
			d["class"] = (int)sys->GetTrackedDeviceClass(i);
			d["role"] = (int)sys->GetControllerRoleForTrackedDeviceIndex(i);
			d["ActivityLevel"] = (int)sys->GetTrackedDeviceActivityLevel(i);
			auto str = [&](const char* name, vr::ETrackedDeviceProperty prop) {
				char value[256] = {};
				vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
				sys->GetStringTrackedDeviceProperty(i, prop, value, sizeof(value), &pe);
				d[name] = value;
				d[std::string(name) + " error"] = (int)pe;
			};
			str("InputProfilePath", vr::Prop_InputProfilePath_String);
			str("RegisteredDeviceType", vr::Prop_RegisteredDeviceType_String);
			str("ControllerType", vr::Prop_ControllerType_String);
			str("RenderModelName", vr::Prop_RenderModelName_String);
			vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
			d["HandSelectionPriority"] = sys->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerHandSelectionPriority_Int32, &pe);
			d["HandSelectionPriority error"] = (int)pe;
			if (i == vr::k_unTrackedDeviceIndex_Hmd) {
				float depth = sys->GetFloatTrackedDeviceProperty(i, vr::Prop_UserHeadToEyeDepthMeters_Float, &pe);
				d["HeadToEyeDepth"] = Milli(depth);
				d["HeadToEyeDepth error"] = (int)pe;
			}
			c.Record("device " + std::to_string(i), d);
		}
		Json::Value roles;
		for (int role = 0; role <= vr::TrackedControllerRole_Max; role++) {
			vr::TrackedDeviceIndex_t i = sys->GetTrackedDeviceIndexForControllerRole((vr::ETrackedControllerRole)role);
			roles[std::to_string(role)] = i == vr::k_unTrackedDeviceIndexInvalid ? -1 : (int)i;
		}
		c.Record("GetTrackedDeviceIndexForControllerRole", roles);
	}
	c.core->Cleanup();
}

// A second Init with another type while a scene app runs, and the Shutdowns that match them
void InitAgainCase(CaseContext& c)
{
	if (!InitScene(c))
		return;
	for (auto type : { vr::VRApplication_Overlay, vr::VRApplication_Background, vr::VRApplication_Utility, vr::VRApplication_Other }) {
		Json::Value r = Ret(c.core->Init(type, nullptr));
		r["IVRSystem after"] = c.core->GetGenericInterface("IVRSystem_022", nullptr) != nullptr;
		c.Record("Init " + std::to_string((int)type) + " while a scene app", r);
		if (r["ret"].asInt() == vr::VRInitError_None)
			c.core->Cleanup();
	}
	c.core->Cleanup();
}

void ChaperoneColoursCase(CaseContext& c)
{
	if (!InitScene(c))
		return;
	auto* chap = Get<vr::IVRChaperone_004::IVRChaperone>(c, "IVRChaperone_004");
	auto* setup = Get<vr::IVRChaperoneSetup_006::IVRChaperoneSetup>(c, "IVRChaperoneSetup_006");
	auto quads = [&](const char* label, bool working) {
		vr::HmdQuad_t q[64];
		uint32_t n = 64;
		Json::Value r;
		r["ret"] = working ? setup->GetWorkingCollisionBoundsInfo(q, &n) : setup->GetLiveCollisionBoundsInfo(q, &n);
		r["count"] = n;
		if (r["ret"].asBool() && n > 0 && n <= 64) {
			for (int k = 0; k < 4; k++) {
				Json::Value v;
				for (int a = 0; a < 3; a++)
					v.append(Milli(q[0].vCorners[k].v[a]));
				r["first quad"].append(v);
			}
		}
		c.Record(label, r);
	};
	if (setup) {
		Frames(c, 5);
		quads("GetLiveCollisionBoundsInfo", false);
		setup->RevertWorkingCopy();
		quads("GetWorkingCollisionBoundsInfo", true);
	}
	if (chap) {
		vr::HmdColor_t colours[2] = {}, camera = {};
		chap->GetBoundsColor(colours, 2, 1.0f, &camera);
		Json::Value r;
		for (const vr::HmdColor_t& col : colours)
			r["colours"].append(Json::Value(std::to_string(Milli(col.r)) + "," + std::to_string(Milli(col.g)) + "," +
			    std::to_string(Milli(col.b)) + "," + std::to_string(Milli(col.a))));
		r["camera a"] = Milli(camera.a);
		r["AreBoundsVisible"] = chap->AreBoundsVisible();
		c.Record("GetBoundsColor", r);
		chap->SetSceneColor(vr::HmdColor_t{ 1, 0, 0, 1 });
		chap->GetBoundsColor(colours, 2, 1.0f, &camera);
		Json::Value r2;
		r2["first r"] = Milli(colours[0].r);
		r2["first g"] = Milli(colours[0].g);
		c.Record("GetBoundsColor after SetSceneColor red", r2);
	}
	c.core->Cleanup();
}

// Submits the compositor refuses: bounds of no area, and an app without focus (an overlay app's)
void SubmitRefusalsCase(CaseContext& c)
{
	callers::Texture tex(ArgSet::Valid, *c.call);
	if (InitScene(c)) {
		auto* comp = Get<vr::IVRCompositor_029::IVRCompositor>(c, "IVRCompositor_029");
		if (comp) {
			vr::TrackedDevicePose_t poses[16];
			comp->WaitGetPoses(poses, 16, nullptr, 0);
			Json::Value r;
			vr::VRTextureBounds_t zero{ 0.5f, 0.5f, 0.5f, 0.5f };
			r["zero-area bounds"] = (int)comp->Submit(vr::Eye_Left, tex.get(), &zero);
			vr::VRTextureBounds_t flipped{ 1, 1, 0, 0 };
			r["flipped bounds"] = (int)comp->Submit(vr::Eye_Right, tex.get(), &flipped);
			c.Record("Submit bounds", r);
		}
		c.core->Cleanup();
	}
	vr::EVRInitError e = c.core->Init(vr::VRApplication_Overlay, nullptr);
	Json::Value r = Ret(e);
	if (e == vr::VRInitError_None) {
		auto* comp = (vr::IVRCompositor_029::IVRCompositor*)c.core->GetGenericInterface("IVRCompositor_029", nullptr);
		r["submit"] = comp ? (int)comp->Submit(vr::Eye_Left, tex.get()) : -1;
		c.core->Cleanup();
	}
	c.Record("Overlay app submit", r);
}

// IVRInput with an action manifest naming skeletons, a pose and a button (manifest_skeleton.json), after some frames
void SkeletalInputCase(CaseContext& c)
{
	namespace in = vr::IVRInput_010;
	if (!InitScene(c))
		return;
	auto* input = Get<in::IVRInput>(c, "IVRInput_010");
	if (!input) {
		c.core->Cleanup();
		return;
	}
	std::string manifest = Utf8Path(c.call->dataDir / "manifest_skeleton.json");
	c.Record("SetActionManifestPath", Ret(input->SetActionManifestPath(manifest.c_str())));
	vr::VRActionSetHandle_t set = 0;
	vr::VRActionHandle_t left = 0, right = 0, pose = 0, grab = 0;
	input->GetActionSetHandle("/actions/main", &set);
	input->GetActionHandle("/actions/main/in/skeleton_left", &left);
	input->GetActionHandle("/actions/main/in/skeleton_right", &right);
	input->GetActionHandle("/actions/main/in/pose", &pose);
	input->GetActionHandle("/actions/main/in/grab", &grab);
	in::VRActiveActionSet_t active{};
	active.ulActionSet = set;
	for (int frame = 0; frame < 10; frame++) {
		Frames(c, 1);
		input->UpdateActionState(&active, sizeof(active), 1);
	}

	for (auto [action, hand] : { std::pair{ left, "left" }, std::pair{ right, "right" } }) {
		const std::string h = hand;
		in::InputSkeletalActionData_t data{};
		Json::Value a = Ret(input->GetSkeletalActionData(action, &data, sizeof(data)));
		a["bActive"] = data.bActive;
		c.Record(h + " GetSkeletalActionData", a);
		for (auto range : { vr::VRSkeletalMotionRange_WithController, vr::VRSkeletalMotionRange_WithoutController }) {
			vr::VRBoneTransform_t bones[31] = {};
			Json::Value b = Ret(input->GetSkeletalBoneData(action, in::VRSkeletalTransformSpace_Parent, range, bones, 31));
			// The wrist, and a fingertip's position, which differ between the ranges
			for (int bone : { 1, 9 }) {
				Json::Value p;
				for (int k = 0; k < 3; k++)
					p.append(Milli(bones[bone].position.v[k]));
				b["bone " + std::to_string(bone)] = p;
			}
			c.Record(h + " GetSkeletalBoneData " + (range == vr::VRSkeletalMotionRange_WithController ? "WithController" : "WithoutController"), b);
		}
		for (auto type : { in::VRSummaryType_FromAnimation, in::VRSummaryType_FromDevice }) {
			in::VRSkeletalSummaryData_t s{};
			Json::Value r = Ret(input->GetSkeletalSummaryData(action, type, &s));
			for (float curl : s.flFingerCurl)
				r["curl"].append(Milli(curl));
			for (float splay : s.flFingerSplay)
				r["splay"].append(Milli(splay));
			c.Record(h + " GetSkeletalSummaryData " + (type == in::VRSummaryType_FromAnimation ? "FromAnimation" : "FromDevice"), r);
		}
	}

	in::InputPoseActionData_t pd{};
	Json::Value p = Ret(input->GetPoseActionDataForNextFrame(pose, vr::TrackingUniverseStanding, &pd, sizeof(pd), vr::k_ulInvalidInputValueHandle));
	p["bActive"] = pd.bActive;
	p["bPoseIsValid"] = pd.pose.bPoseIsValid;
	c.Record("GetPoseActionDataForNextFrame", p);

	vr::VRInputValueHandle_t origins[16] = {};
	Json::Value o = Ret(input->GetActionOrigins(set, grab, origins, 16));
	int count = 0;
	for (vr::VRInputValueHandle_t h : origins)
		count += h != vr::k_ulInvalidInputValueHandle;
	o["count"] = count;
	c.Record("GetActionOrigins", o);
	for (int32_t sections : { (int32_t)in::VRInputString_Hand, (int32_t)in::VRInputString_ControllerType,
	         (int32_t)in::VRInputString_InputSource, (int32_t)in::VRInputString_All }) {
		char name[256] = {};
		Json::Value n = Ret(count ? input->GetOriginLocalizedName(origins[0], name, sizeof(name), sections) : -1);
		n["name"] = name;
		c.Record("GetOriginLocalizedName " + std::to_string(sections), n);
	}

	std::string other = Utf8Path(c.call->dataDir / "manifest_basic.json");
	Json::Value again = Ret(input->SetActionManifestPath(other.c_str()));
	vr::VRActionHandle_t trigger = 0;
	again["trigger handle"] = input->GetActionHandle("/actions/main/in/trigger", &trigger);
	c.Record("SetActionManifestPath again, another file", again);
	c.core->Cleanup();
}

// Legacy input: an Index controller's finger curls (axes 3 and 4) and haptic pulses of a few strengths
void LegacyControllerCase(CaseContext& c)
{
	if (!InitScene(c))
		return;
	auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	if (sys) {
		Frames(c, 10);
		for (auto role : { vr::TrackedControllerRole_LeftHand, vr::TrackedControllerRole_RightHand }) {
			const std::string hand = role == vr::TrackedControllerRole_LeftHand ? "left" : "right";
			vr::TrackedDeviceIndex_t i = sys->GetTrackedDeviceIndexForControllerRole(role);
			if (i == vr::k_unTrackedDeviceIndexInvalid) {
				c.Record(hand + " no controller");
				continue;
			}
			vr::VRControllerState_t s{};
			Json::Value r;
			r["ret"] = sys->GetControllerState(i, &s, sizeof(s));
			for (int axis = 0; axis < (int)vr::k_unControllerStateAxisCount; axis++)
				r["axis types"].append(sys->GetInt32TrackedDeviceProperty(i, (vr::ETrackedDeviceProperty)(vr::Prop_Axis0Type_Int32 + axis)));
			r["axis 3 x"] = Milli(s.rAxis[3].x);
			r["axis 4 x"] = Milli(s.rAxis[4].x);
			c.Record(hand + " GetControllerState", r);
			for (unsigned short us : { 0, 500, 3999, 4000, 65535 })
				sys->TriggerHapticPulse(i, 0, us);
			c.Record(hand + " TriggerHapticPulse strengths");
		}
	}
	c.core->Cleanup();
}

// Each controller's render model: its name, and each component's state through the device path
void RenderModelComponentsCase(CaseContext& c)
{
	namespace rm = vr::IVRRenderModels_006;
	if (!InitScene(c))
		return;
	auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	auto* models = Get<rm::IVRRenderModels>(c, "IVRRenderModels_006");
	auto* input = Get<vr::IVRInput_010::IVRInput>(c, "IVRInput_010");
	if (sys && models && input) {
		Frames(c, 10);
		for (const char* hand : { "left", "right" }) {
			vr::TrackedDeviceIndex_t i = sys->GetTrackedDeviceIndexForControllerRole(
			    std::string(hand) == "left" ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
			Json::Value r;
			char model[256] = {};
			if (i != vr::k_unTrackedDeviceIndexInvalid)
				sys->GetStringTrackedDeviceProperty(i, vr::Prop_RenderModelName_String, model, sizeof(model));
			r["model"] = model;
			vr::VRInputValueHandle_t path = 0;
			input->GetInputSourceHandle((std::string("/user/hand/") + hand).c_str(), &path);
			uint32_t n = model[0] ? models->GetComponentCount(model) : 0;
			r["components"] = n;
			for (uint32_t k = 0; k < n && k < 64; k++) {
				char comp[128] = {};
				models->GetComponentName(model, k, comp, sizeof(comp));
				rm::RenderModel_ControllerMode_State_t mode{};
				for (bool scroll : { false, true }) {
					mode.bScrollWheelVisible = scroll;
					vr::RenderModel_ComponentState_t state{};
					bool ok = models->GetComponentStateForDevicePath(model, comp, path, &mode, &state);
					r[std::string(comp) + (scroll ? " scroll" : "")] = ok ? (int)state.uProperties : -1;
				}
			}
			c.Record(std::string(hand) + " render model components", r);
		}
		Json::Value names;
		char name[256];
		for (uint32_t k = 0; k < 256 && models->GetRenderModelName(k, name, sizeof(name)); k++)
			names["count"] = k + 1;
		c.Record("GetRenderModelName", names);
	}
	c.core->Cleanup();
}

// A new overlay's defaults, its flags, and the events its changes send
void OverlayDefaultsCase(CaseContext& c)
{
	namespace ov = vr::IVROverlay_028;
	if (!InitScene(c))
		return;
	auto* overlay = Get<ov::IVROverlay>(c, "IVROverlay_028");
	auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	if (overlay && sys) {
		Frames(c, 2);
		vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
		c.Record("CreateOverlay", Ret(overlay->CreateOverlay("oc.defaults", "Defaults", &h)));
		Json::Value d;
		float f = -1;
		overlay->GetOverlayCurvature(h, &f);
		d["curvature"] = Milli(f);
		overlay->GetOverlayWidthInMeters(h, &f);
		d["width"] = Milli(f);
		overlay->GetOverlayAlpha(h, &f);
		d["alpha"] = Milli(f);
		uint32_t order = 99;
		overlay->GetOverlaySortOrder(h, &order);
		d["sort order"] = order;
		for (auto [flag, name] : { std::pair{ ov::VROverlayFlags_SideBySide_Parallel, "SideBySide_Parallel" },
		         std::pair{ ov::VROverlayFlags_SideBySide_Crossed, "SideBySide_Crossed" },
		         std::pair{ ov::VROverlayFlags_Panorama, "Panorama" }, std::pair{ ov::VROverlayFlags_StereoPanorama, "StereoPanorama" } }) {
			bool on = true;
			overlay->GetOverlayFlag(h, flag, &on);
			d[name] = on;
			Json::Value set = Ret(overlay->SetOverlayFlag(h, flag, true));
			overlay->GetOverlayFlag(h, flag, &on);
			set["after"] = on;
			overlay->SetOverlayFlag(h, flag, false);
			d[std::string(name) + " set"] = set;
		}
		c.Record("new overlay defaults", d);

		// The events of a name change, a show and a hide, then the destroy (on the system's queue)
		overlay->SetOverlayName(h, "Renamed");
		overlay->ShowOverlay(h);
		Frames(c, 2);
		overlay->HideOverlay(h);
		Json::Value events;
		vr::VREvent_t ev;
		for (int k = 0; k < 64 && overlay->PollNextOverlayEvent(h, (vr::VREvent_t*)&ev, sizeof(ev)); k++)
			events["overlay"].append((int)ev.eventType);
		overlay->DestroyOverlay(h);
		Frames(c, 2);
		for (int k = 0; k < 64 && sys->PollNextEvent(&ev, sizeof(ev)); k++)
			events["system"].append((int)ev.eventType);
		c.Record("overlay events", events);
	}
	c.core->Cleanup();
}

// The defaults of keys games read, and the event a change of one sends
void SettingsDefaultsCase(CaseContext& c)
{
	namespace st = vr::IVRSettings_003;
	if (!InitScene(c))
		return;
	auto* settings = Get<st::IVRSettings>(c, "IVRSettings_003");
	auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
	if (settings && sys) {
		auto num = [&](const char* section, const char* key) {
			st::EVRSettingsError e = st::VRSettingsError_None;
			Json::Value r;
			r["float"] = Milli(settings->GetFloat(section, key, &e));
			r["error"] = (int)e;
			c.Record(std::string(section) + "/" + key, r);
		};
		auto text = [&](const char* section, const char* key) {
			st::EVRSettingsError e = st::VRSettingsError_None;
			char value[256] = {};
			settings->GetString(section, key, value, sizeof(value), &e);
			Json::Value r;
			r["empty"] = value[0] == 0;
			r["error"] = (int)e;
			c.Record(std::string(section) + "/" + key, r);
		};
		num("collisionBounds", "CollisionBoundsFadeDistance");
		num("collisionBounds", "CollisionBoundsColorGammaA");
		num("steamvr", "supersampleScale");
		num("steamvr", "ipd");
		text("audio", "playbackDevice");
		text("audio", "recordingDevice");
		text("steamvr", "background");

		Frames(c, 2);
		settings->SetFloat("collisionBounds", "CollisionBoundsFadeDistance", 0.5f);
		Frames(c, 3);
		Json::Value r;
		vr::VREvent_t ev;
		for (int k = 0; k < 64 && sys->PollNextEvent(&ev, sizeof(ev)); k++) {
			if (ev.eventType >= vr::VREvent_BackgroundSettingHasChanged && ev.eventType < vr::VREvent_StatusUpdate) {
				Json::Value e;
				e["type"] = (int)ev.eventType;
				e["device"] = ev.trackedDeviceIndex;
				r["events"].append(e);
			}
		}
		c.Record("SetFloat collisionBounds fade distance: events", r);
		settings->RemoveKeyInSection("collisionBounds", "CollisionBoundsFadeDistance");
	}
	c.core->Cleanup();
}

// Where the installed applications are listed from: their count, and the first one's source and binary
void ApplicationListCase(CaseContext& c)
{
	namespace ap = vr::IVRApplications_007;
	if (!InitScene(c))
		return;
	auto* apps = Get<ap::IVRApplications>(c, "IVRApplications_007");
	if (apps) {
		Json::Value r;
		uint32_t n = apps->GetApplicationCount();
		r["count"] = n;
		char key[256] = {};
		if (n) {
			r["first key"] = (int)apps->GetApplicationKeyByIndex(0, key, sizeof(key));
			ap::EVRApplicationError e = ap::VRApplicationError_None;
			char source[256] = {}, binary[1024] = {};
			apps->GetApplicationPropertyString(key, ap::VRApplicationProperty_Source_String, source, sizeof(source), &e);
			r["source error"] = (int)e;
			r["source"] = source;
			apps->GetApplicationPropertyString(key, ap::VRApplicationProperty_BinaryPath_String, binary, sizeof(binary), &e);
			r["binary path set"] = binary[0] != 0;
		}
		c.Record("GetApplicationCount", r);
	}
	c.core->Cleanup();
}

// IVRIOBuffer's modes: a buffer created, written, opened again for reading, read, and its property container
void IOBufferCase(CaseContext& c)
{
	namespace io = vr::IVRIOBuffer_002;
	if (!InitScene(c))
		return;
	auto* buffers = Get<io::IVRIOBuffer>(c, "IVRIOBuffer_002");
	if (buffers) {
		io::IOBufferHandle_t w = 0, r = 0;
		const char* path = "/oc/conformance/buffer";
		c.Record("Open create write", Ret(buffers->Open(path, (io::EIOBufferMode)(io::IOBufferMode_Write | io::IOBufferMode_Create), 16, 4, &w)));
		c.Record("Open read", Ret(buffers->Open(path, io::IOBufferMode_Read, 16, 4, &r)));
		Json::Value h;
		h["HasReaders"] = w ? buffers->HasReaders(w) : false;
		h["PropertyContainer not 0"] = w ? buffers->PropertyContainer(w) != 0 : false;
		c.Record("HasReaders", h);
		char data[16] = "conformance";
		c.Record("Write", Ret(w ? buffers->Write(w, data, sizeof(data)) : -1));
		char back[16] = {};
		uint32_t read = 0;
		Json::Value rd = Ret(r ? buffers->Read(r, back, sizeof(back), &read) : -1);
		rd["read"] = read;
		rd["same"] = memcmp(back, data, sizeof(data)) == 0;
		c.Record("Read", rd);
		c.Record("Open missing for reading", Ret(buffers->Open("/oc/conformance/none", io::IOBufferMode_Read, 16, 4, &r)));
		if (w)
			c.Record("Close write", Ret(buffers->Close(w)));
	}
	c.core->Cleanup();
}

// An overlay app and a background app that read the controllers through IVRSystem only and submit no frames, as
// overlay tools do: run with scripts/helper_apps_controllers.json on the stand-in
void HelperAppControllersCase(CaseContext& c)
{
	for (auto [type, name] : { std::pair{ vr::VRApplication_Overlay, "overlay" }, std::pair{ vr::VRApplication_Background, "background" } }) {
		const std::string t = name;
		vr::EVRInitError e = c.core->Init(type, nullptr);
		if (e != vr::VRInitError_None) {
			c.Record(t + " Init", Ret(e));
			continue;
		}
		auto* sys = Get<vr::IVRSystem_022::IVRSystem>(c, "IVRSystem_022");
		if (sys) {
			bool connected[3] = {};
			vr::TrackedDevicePose_t poses[3] = {};
			vr::VRControllerState_t state{};
			bool stateRet = false;
			for (int i = 0; i < 60 && !(connected[1] && connected[2] && poses[1].bPoseIsValid && poses[2].bPoseIsValid); i++) {
				std::this_thread::sleep_for(std::chrono::milliseconds(11));
				for (vr::TrackedDeviceIndex_t d = 1; d <= 2; d++)
					connected[d] = sys->IsTrackedDeviceConnected(d);
				sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, 3);
				vr::TrackedDeviceIndex_t left = sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
				stateRet = left != vr::k_unTrackedDeviceIndexInvalid && sys->GetControllerState(left, &state, sizeof(state));
				vr::VREvent_t ev;
				for (int k = 0; k < 64 && sys->PollNextEvent(&ev, sizeof(ev)); k++) {
				}
			}
			Json::Value r;
			r["left connected"] = connected[1];
			r["right connected"] = connected[2];
			r["left pose valid"] = poses[1].bPoseIsValid;
			r["right pose valid"] = poses[2].bPoseIsValid;
			r["GetControllerState"] = stateRet;
			c.Record(t + " controllers", r);
		}
		c.core->Cleanup();
	}
}

} // namespace

const std::map<std::string, CaseFn>& conformance::SteamVRRunCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "device-properties", DevicePropertiesCase },
		{ "init-again", InitAgainCase },
		{ "chaperone-colours", ChaperoneColoursCase },
		{ "submit-refusals", SubmitRefusalsCase },
		{ "skeletal-input", SkeletalInputCase },
		{ "legacy-controller", LegacyControllerCase },
		{ "render-model-components", RenderModelComponentsCase },
		{ "overlay-defaults", OverlayDefaultsCase },
		{ "settings-defaults", SettingsDefaultsCase },
		{ "application-list", ApplicationListCase },
		{ "io-buffer", IOBufferCase },
		{ "helper-app-controllers", HelperAppControllersCase },
	};
	return cases;
}
