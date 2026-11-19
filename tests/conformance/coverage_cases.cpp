// Cases for entry points and interfaces the per-method callers can't reach: the C exports beside VRClientCoreFactory,
// the client core's other methods and its older version, a held init token and interface pointer, the interfaces a
// Utility or Overlay app is given, IVRMailbox, IVRNotifications_002, and the state interfaces across a Shutdown and
// Init. Each writes one record per step, as cases.cpp's do; none needs a headset.

#include "conformance.h"

#include "IVRDebug_001.h"
#include "IVRDriverManager_001.h"
#include "IVRNotifications_002.h"
#include "IVROverlay_028.h"
#include "IVRResources_001.h"
#include "IVRSettings_003.h"
#include "IVRSpatialAnchors_001.h"
#include "IVRSystem_022.h"
#include "custom_interfaces/IVRMailbox_001.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace conformance;

namespace conformance {

// IVRClientCore_002: the same methods as IVRClientCore_003 but for an Init that takes no startup info
// Not in the anonymous namespace: a class nothing derives from there would let the compiler assume it is never called
class ClientCore002 {
public:
	virtual vr::EVRInitError Init(vr::EVRApplicationType eApplicationType) = 0;
	virtual void Cleanup() = 0;
	virtual vr::EVRInitError IsInterfaceVersionValid(const char* pchInterfaceVersion) = 0;
	virtual void* GetGenericInterface(const char* pchNameAndVersion, vr::EVRInitError* peError) = 0;
	virtual bool BIsHmdPresent() = 0;
	virtual const char* GetEnglishStringForHmdError(vr::EVRInitError eError) = 0;
	virtual const char* GetIDForVRInitError(vr::EVRInitError eError) = 0;
};

} // namespace conformance

namespace {

#ifdef _WIN32
#define OC_C_CALL __cdecl
#else
#define OC_C_CALL
#endif

Json::Value Err(int e)
{
	Json::Value v;
	v["ret"] = e;
	return v;
}

Json::Value Text(const char* s)
{
	return s ? Json::Value(std::string(s)) : Json::Value("null");
}

// The versions asked about: ours, one nobody has, a function table's, an empty name
const char* const kVersionNames[] = { "IVRSystem_022", "IVRSystem_999", "FnTable:IVRSystem_022", "FnTable:IVRNope_001",
	"IVRPaths_002", "IVRMailbox_001", "IVRControlPanel_006", "IVRSettings_003", "" };
// Init errors: none, the first, the common ones, the last of SteamVR's and values beyond it
const int kErrors[] = { 0, 1, 100, 105, 108, 109, 124, 130, 146, 147, 200, 99999 };

template <typename Core>
void CoreMethods(CaseContext& c, Core* core, const std::string& label)
{
	for (const char* name : kVersionNames) {
		Json::Value r;
		r["ret"] = (int)core->IsInterfaceVersionValid(name);
		c.Record(label + " IsInterfaceVersionValid(" + name + ")", r);
	}
	for (int e : kErrors) {
		Json::Value r;
		r["english"] = Text(core->GetEnglishStringForHmdError((vr::EVRInitError)e));
		r["id"] = Text(core->GetIDForVRInitError((vr::EVRInitError)e));
		c.Record(label + " error " + std::to_string(e), r);
	}
}

// IVRClientCore_003's methods that no generic caller reaches (the client core isn't a generated interface), and
// IVRClientCore_002 from the factory: its Init, and the interfaces it hands out
void ClientCoreMethodsCase(CaseContext& c)
{
	CoreMethods(c, c.core, "IVRClientCore_003");
	Json::Value r;
	r["ret"] = c.core->BIsHmdPresent();
	c.Record("IVRClientCore_003 BIsHmdPresent", r);

	int rc = 0;
	auto* old = c.factory ? (conformance::ClientCore002*)c.factory("IVRClientCore_002", &rc) : nullptr;
	r = Json::Value();
	r["found"] = old != nullptr;
	r["return_code"] = rc;
	c.Record("IVRClientCore_002", r);
	if (!old)
		return;
	CoreMethods(c, old, "IVRClientCore_002");
	r = Json::Value();
	r["ret"] = old->BIsHmdPresent();
	c.Record("IVRClientCore_002 BIsHmdPresent", r);

	vr::EVRInitError e = old->Init(vr::VRApplication_Scene);
	r = Json::Value();
	r["init_error"] = (int)e;
	if (e == vr::VRInitError_None) {
		vr::EVRInitError le = vr::VRInitError_None;
		void* sys = old->GetGenericInterface("IVRSystem_022", &le);
		r["system_found"] = sys != nullptr;
		r["lookup_error"] = (int)le;
		void* missing = old->GetGenericInterface("IVRNope_001", &le);
		r["missing_found"] = missing != nullptr;
		r["missing_error"] = (int)le;
	}
	c.Record("IVRClientCore_002 Init", r);
	if (e == vr::VRInitError_None)
		old->Cleanup();
}

using IsValidFn = bool(OC_C_CALL*)(const char*);
using BoolFn = bool(OC_C_CALL*)();
using TokenFn = uint32_t(OC_C_CALL*)();
using ErrorTextFn = const char*(OC_C_CALL*)(vr::EVRInitError);
using HmdErrorTextFn = char*(OC_C_CALL*)(int);
using PathFn = const char*(OC_C_CALL*)();
using PathCopyFn = bool(OC_C_CALL*)(char*, uint32_t, uint32_t*);
using InitFn = uint32_t(OC_C_CALL*)(vr::EVRInitError*, vr::EVRApplicationType);
using Init2Fn = uint32_t(OC_C_CALL*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using VoidFn = void(OC_C_CALL*)();
using GenericFn = void*(OC_C_CALL*)(const char*, vr::EVRInitError*);
using AccessorFn = void*(OC_C_CALL*)();
using LegacyInitFn = void*(OC_C_CALL*)(vr::EVRInitError*, vr::EVRApplicationType);

template <typename Fn>
Fn Find(CaseContext& c, const char* name)
{
	return c.symbol ? (Fn)c.symbol(name) : nullptr;
}

// The functions openvr_api exports besides VRClientCoreFactory. A runtime that lacks one (SteamVR's vrclient has
// none of them) is recorded as not found.
void CApiExportsCase(CaseContext& c)
{
	auto isValid = Find<IsValidFn>(c, "VR_IsInterfaceVersionValid");
	auto installed = Find<BoolFn>(c, "VR_IsRuntimeInstalled");
	auto token = Find<TokenFn>(c, "VR_GetInitToken");
	auto symbolOf = Find<ErrorTextFn>(c, "VR_GetVRInitErrorAsSymbol");
	auto englishOf = Find<ErrorTextFn>(c, "VR_GetVRInitErrorAsEnglishDescription");
	auto hmdText = Find<HmdErrorTextFn>(c, "VR_GetStringForHmdError");
	auto runtimePath = Find<PathFn>(c, "VR_RuntimePath");
	auto copyPath = Find<PathCopyFn>(c, "VR_GetRuntimePath");
	auto hmdPresent = Find<BoolFn>(c, "VR_IsHmdPresent");

	Json::Value r;
	r["VR_IsInterfaceVersionValid"] = isValid != nullptr;
	r["VR_IsRuntimeInstalled"] = installed != nullptr;
	r["VR_IsHmdPresent"] = hmdPresent != nullptr;
	r["VR_GetInitToken"] = token != nullptr;
	r["VR_GetVRInitErrorAsSymbol"] = symbolOf != nullptr;
	r["VR_GetVRInitErrorAsEnglishDescription"] = englishOf != nullptr;
	r["VR_GetStringForHmdError"] = hmdText != nullptr;
	r["VR_RuntimePath"] = runtimePath != nullptr;
	r["VR_GetRuntimePath"] = copyPath != nullptr;
	r["HmdSystemFactory"] = Find<void*>(c, "HmdSystemFactory") != nullptr;
	r["VRControlPanel"] = Find<void*>(c, "VRControlPanel") != nullptr;
	r["VR_Init"] = Find<void*>(c, "VR_Init") != nullptr;
	r["VR_Shutdown"] = Find<void*>(c, "VR_Shutdown") != nullptr;
	c.Record("exports", r);

	if (isValid) {
		for (const char* name : kVersionNames) {
			r = Json::Value();
			r["ret"] = isValid(name);
			c.Record(std::string("VR_IsInterfaceVersionValid(") + name + ")", r);
		}
	}
	if (installed) {
		r = Json::Value();
		r["ret"] = installed();
		c.Record("VR_IsRuntimeInstalled", r);
	}
	if (hmdPresent) {
		r = Json::Value();
		r["ret"] = hmdPresent();
		c.Record("VR_IsHmdPresent", r);
	}
	if (symbolOf || englishOf || hmdText) {
		for (int e : kErrors) {
			r = Json::Value();
			if (symbolOf)
				r["symbol"] = Text(symbolOf((vr::EVRInitError)e));
			if (englishOf)
				r["english"] = Text(englishOf((vr::EVRInitError)e));
			if (hmdText)
				r["hmd_text"] = Text(hmdText(e));
			c.Record("init error " + std::to_string(e), r);
		}
	}
	if (runtimePath) {
		r = Json::Value();
		const char* path = runtimePath();
		r["not_empty"] = path && *path;
		c.Record("VR_RuntimePath", r);
	}
	if (copyPath) {
		// The size it needs, a buffer too small for it, one that fits, and no buffer or size pointer
		uint32_t needed = 0;
		std::vector<char> big(4096, 'x');
		r = Json::Value();
		r["null_buffer"] = copyPath(nullptr, 0, &needed);
		r["needed"] = needed;
		uint32_t again = 0;
		char one[1] = { 'x' };
		r["small_ret"] = copyPath(one, 1, &again);
		r["small_needed_same"] = again == needed;
		uint32_t fits = 0;
		r["fits_ret"] = copyPath(big.data(), (uint32_t)big.size(), &fits);
		r["fits_size"] = fits == needed;
		r["fits_terminated"] = needed > 0 && needed <= big.size() && big[needed - 1] == '\0';
		r["no_size_pointer"] = copyPath(big.data(), (uint32_t)big.size(), nullptr);
		c.Record("VR_GetRuntimePath", r);
	}

	// An Init through the exports, the way an application's openvr_api calls them
	auto init = Find<InitFn>(c, "VR_InitInternal");
	auto init2 = Find<Init2Fn>(c, "VR_InitInternal2");
	auto shutdown = Find<VoidFn>(c, "VR_ShutdownInternal");
	auto generic = Find<GenericFn>(c, "VR_GetGenericInterface");
	auto controlPanel = Find<AccessorFn>(c, "VRControlPanel");
	if (token) {
		r = Json::Value();
		r["before_init"] = token() == 0 ? "zero" : "set";
		c.Record("VR_GetInitToken before Init", r);
	}
	if (controlPanel) {
		r = Json::Value();
		r["before_init_null"] = controlPanel() == nullptr;
		c.Record("VRControlPanel before Init", r);
	}
	if (init && shutdown) {
		vr::EVRInitError e = vr::VRInitError_Unknown;
		uint32_t first = init(&e, vr::VRApplication_Scene);
		r = Json::Value();
		r["init_error"] = (int)e;
		r["token_returned"] = first != 0;
		if (e == vr::VRInitError_None) {
			if (token)
				r["token_matches"] = token() == first;
			if (generic) {
				vr::EVRInitError le = vr::VRInitError_None;
				r["system_found"] = generic("IVRSystem_022", &le) != nullptr;
				r["lookup_error"] = (int)le;
				r["missing_found"] = generic("IVRNope_001", &le) != nullptr;
				r["missing_error"] = (int)le;
			}
			if (controlPanel)
				r["control_panel_found"] = controlPanel() != nullptr;
			shutdown();
		}
		c.Record("VR_InitInternal", r);
	}
	if (init2 && shutdown) {
		vr::EVRInitError e = vr::VRInitError_Unknown;
		uint32_t t = init2(&e, vr::VRApplication_Scene, nullptr);
		r = Json::Value();
		r["init_error"] = (int)e;
		r["token_returned"] = t != 0;
		if (e == vr::VRInitError_None)
			shutdown();
		c.Record("VR_InitInternal2", r);
	}
	if (generic) {
		// After Shutdown the interfaces are gone again
		vr::EVRInitError le = vr::VRInitError_None;
		r = Json::Value();
		r["found"] = generic("IVRSystem_022", &le) != nullptr;
		r["lookup_error"] = (int)le;
		c.Record("VR_GetGenericInterface after Shutdown", r);
	}

	// The accessors of the older API (Windows only: they are exported where an application of that SDK can import them)
	auto legacyInit = Find<LegacyInitFn>(c, "VR_Init");
	auto legacyShutdown = Find<VoidFn>(c, "VR_Shutdown");
	if (legacyInit && legacyShutdown) {
		vr::EVRInitError e = vr::VRInitError_Unknown;
		void* sys = legacyInit(&e, vr::VRApplication_Scene);
		r = Json::Value();
		r["init_error"] = (int)e;
		r["system_found"] = sys != nullptr;
		for (const char* name : { "VRSystem", "VRChaperone", "VRCompositor", "VROverlay", "VRRenderModels", "VRSettings" }) {
			auto accessor = Find<AccessorFn>(c, name);
			r[std::string(name) + "_exported"] = accessor != nullptr;
			if (accessor && e == vr::VRInitError_None)
				r[std::string(name) + "_found"] = accessor() != nullptr;
		}
		if (e == vr::VRInitError_None)
			legacyShutdown();
		c.Record("VR_Init", r);
	}
}

// What an application holds across Init and Cleanup: the init token and its interface pointers. A second Init while
// one is running is counted, and the first Cleanup leaves the interfaces usable.
void InitHeldCase(CaseContext& c)
{
	auto token = Find<TokenFn>(c, "VR_GetInitToken");
	auto lookup = [&](vr::EVRInitError* le) {
		return (vr::IVRSystem_022::IVRSystem*)c.core->GetGenericInterface("IVRSystem_022", le);
	};
	auto width = [](vr::IVRSystem_022::IVRSystem* sys) {
		uint32_t w = 0, h = 0;
		if (sys)
			sys->GetRecommendedRenderTargetSize(&w, &h);
		return (Json::UInt)w;
	};

	Json::Value r;
	if (token)
		r["token_before"] = token() == 0 ? "zero" : "set";
	c.Record("before Init", r);

	r = Json::Value();
	vr::EVRInitError e = c.core->Init(vr::VRApplication_Scene, nullptr);
	r["init_error"] = (int)e;
	if (e != vr::VRInitError_None) {
		c.Record("first Init", r);
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* first = lookup(&le);
	uint32_t firstToken = token ? token() : 0;
	r["token_set"] = firstToken != 0;
	r["found"] = first != nullptr;
	r["width_nonzero"] = width(first) != 0;
	c.Record("first Init", r);
	c.core->Cleanup();

	r = Json::Value();
	if (token)
		r["token_after_cleanup_unchanged"] = token() == firstToken;
	e = c.core->Init(vr::VRApplication_Scene, nullptr);
	r["init_error"] = (int)e;
	if (e != vr::VRInitError_None) {
		c.Record("second Init", r);
		return;
	}
	auto* second = lookup(&le);
	uint32_t secondToken = token ? token() : 0;
	if (token)
		r["token_changed"] = secondToken != firstToken;
	r["found"] = second != nullptr;
	r["width_nonzero"] = width(second) != 0;
	c.Record("second Init", r);

	// A nested Init: the interface it gives is the running one, and the inner Cleanup doesn't end the session
	r = Json::Value();
	e = c.core->Init(vr::VRApplication_Scene, nullptr);
	r["init_error"] = (int)e;
	if (e == vr::VRInitError_None) {
		auto* inner = lookup(&le);
		if (token)
			r["token_same"] = token() == secondToken;
		r["same_pointer"] = inner == second;
		c.core->Cleanup();
		r["outer_width_nonzero"] = width(second) != 0;
		auto* after = lookup(&le);
		r["lookup_after_inner_cleanup"] = after != nullptr;
		r["lookup_error"] = (int)le;
	}
	c.Record("nested Init", r);
	c.core->Cleanup();

	r = Json::Value();
	auto* gone = lookup(&le);
	r["found"] = gone != nullptr;
	r["lookup_error"] = (int)le;
	c.Record("after the last Cleanup", r);
}

// The interfaces each application type is given, and what its settings say
void AppTypeInterfacesCase(CaseContext& c)
{
	static const char* const names[] = { "IVRSettings_003", "IVRApplications_007", "IVRSystem_022", "IVRCompositor_029",
		"IVROverlay_028", "IVRChaperone_004", "IVRChaperoneSetup_006", "IVRInput_010", "IVRNotifications_002",
		"IVRRenderModels_006", "IVRResources_001", "IVRExtendedDisplay_001", "IVRTrackedCamera_006", "IVRDriverManager_001",
		"IVRDebug_001", "IVRSpatialAnchors_001", "IVRControlPanel_001", "IVRMailbox_001", "IVRPaths_002" };
	struct Type {
		vr::EVRApplicationType type;
		const char* name;
	};
	for (Type t : { Type{ vr::VRApplication_Utility, "Utility" }, Type{ vr::VRApplication_Overlay, "Overlay" },
	         Type{ vr::VRApplication_Background, "Background" } }) {
		vr::EVRInitError e = c.core->Init(t.type, nullptr);
		Json::Value r;
		r["init_error"] = (int)e;
		c.Record(std::string(t.name) + " Init", r);
		if (e != vr::VRInitError_None)
			continue;
		for (const char* name : names) {
			vr::EVRInitError le = vr::VRInitError_None;
			void* p = c.core->GetGenericInterface(name, &le);
			r = Json::Value();
			r["found"] = p != nullptr;
			r["lookup_error"] = (int)le;
			c.Record(std::string(t.name) + " " + name, r);
		}
		vr::EVRInitError settingsError = vr::VRInitError_None;
		auto* settings = (vr::IVRSettings_003::IVRSettings*)c.core->GetGenericInterface("IVRSettings_003", &settingsError);
		if (settings) {
			// A settings interface of its own: read a value, write one and read it back
			r = Json::Value();
			vr::IVRSettings_003::EVRSettingsError se = vr::IVRSettings_003::VRSettingsError_None;
			r["missing_float"] = settings->GetFloat("oc.coverage", "missing", &se);
			r["missing_error"] = (int)se;
			settings->SetInt32("oc.coverage", "number", 7, &se);
			r["set_error"] = (int)se;
			r["read_back"] = settings->GetInt32("oc.coverage", "number", &se);
			r["read_error"] = (int)se;
			settings->RemoveSection("oc.coverage", &se);
			c.Record(std::string(t.name) + " IVRSettings_003 use", r);
		}
		c.core->Cleanup();
	}
}

// IVRMailbox_001: registering, the ready message every mailbox starts with, sending and unregistering
void MailboxCase(CaseContext& c)
{
	using namespace vr::IVRMailbox_001;
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* box = (IVRMailbox*)c.core->GetGenericInterface("IVRMailbox_001", &le);
	if (!box) {
		Json::Value r;
		r["lookup_error"] = (int)le;
		c.Record("IVRMailbox_001 missing", r);
		c.core->Cleanup();
		return;
	}

	Json::Value r;
	mbox_handle a = 0, again = 0, other = 0, none = 7;
	r["register"] = (int)box->RegisterMailbox("oc.coverage.a", &a);
	r["handle_set"] = a != 0;
	r["same_name_same_handle"] = box->RegisterMailbox("oc.coverage.a", &again) == 0 && again == a;
	box->RegisterMailbox("oc.coverage.b", &other);
	r["other_name_other_handle"] = other != a && other != 0;
	r["null_name"] = (int)box->RegisterMailbox(nullptr, &none);
	r["null_name_handle"] = (Json::UInt64)none;
	c.Record("register", r);

	r = Json::Value();
	char small[4] = {};
	uint32_t length = 0;
	r["read_small"] = (int)box->ReadMessage(a, small, sizeof(small), &length);
	r["read_small_length"] = length;
	char text[256] = {};
	r["read"] = (int)box->ReadMessage(a, text, sizeof(text), &length);
	r["message"] = text;
	r["length_is_text"] = length == strlen(text);
	r["read_again"] = (int)box->ReadMessage(a, text, sizeof(text), &length);
	r["read_again_length"] = length;
	c.Record("read", r);

	r = Json::Value();
	r["send"] = (int)box->SendMessage(a, "oc.type", "{\"value\":1}");
	r["send_unknown_handle"] = (int)box->SendMessage(a + 1000, "oc.type", "{}");
	r["read_after_send"] = (int)box->ReadMessage(a, text, sizeof(text), &length);
	r["unregister"] = (int)box->UnregisterMailbox(a);
	r["unregister_again"] = (int)box->UnregisterMailbox(a);
	mbox_handle later = 0;
	box->RegisterMailbox("oc.coverage.a", &later);
	r["register_after_unregister_set"] = later != 0;
	box->UnregisterMailbox(later);
	box->UnregisterMailbox(other);
	c.Record("send and unregister", r);

	c.core->Cleanup();
}

// IVRNotifications_002 with an overlay of the program's own: the ids it gives, removing them, and what it refuses
void NotificationsCase(CaseContext& c)
{
	using namespace vr::IVRNotifications_002;
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* notes = (IVRNotifications*)c.core->GetGenericInterface("IVRNotifications_002", &le);
	auto* ov = (vr::IVROverlay_028::IVROverlay*)c.core->GetGenericInterface("IVROverlay_028", &le);
	if (!notes || !ov) {
		c.Record("an interface is missing", Err(le));
		c.core->Cleanup();
		return;
	}
	vr::VROverlayHandle_t oh = vr::k_ulOverlayHandleInvalid;
	Json::Value r;
	r["create_overlay"] = (int)ov->CreateOverlay("oc.coverage.notes", "Notes", &oh);
	c.Record("overlay", r);

	r = Json::Value();
	VRNotificationId first = 0, second = 0, bitmapped = 0, unknown = 5;
	r["transient"] = (int)notes->CreateNotification(oh, 11, EVRNotificationType_Transient, "first\nsecond line", EVRNotificationStyle_Application, nullptr, &first);
	r["first_id_set"] = first != 0;
	r["persistent"] = (int)notes->CreateNotification(oh, 12, EVRNotificationType_Persistent, "second", EVRNotificationStyle_None, nullptr, &second);
	r["ids_differ"] = second != 0 && second != first;
	uint8_t pixels[2 * 2 * 4] = {};
	NotificationBitmap_t bitmap;
	bitmap.m_pImageData = pixels;
	bitmap.m_nWidth = 2;
	bitmap.m_nHeight = 2;
	bitmap.m_nBytesPerPixel = 4;
	r["with_bitmap"] = (int)notes->CreateNotification(oh, 13, EVRNotificationType_Transient, "bitmap", EVRNotificationStyle_Application, &bitmap, &bitmapped);
	r["null_text"] = (int)notes->CreateNotification(oh, 14, EVRNotificationType_Transient, nullptr, EVRNotificationStyle_Application, nullptr, &unknown);
	r["null_id_pointer"] = (int)notes->CreateNotification(oh, 15, EVRNotificationType_Transient, "no id", EVRNotificationStyle_Application, nullptr, nullptr);
	c.Record("create", r);

	r = Json::Value();
	VRNotificationId id = 9;
	r["invalid_overlay"] = (int)notes->CreateNotification(vr::k_ulOverlayHandleInvalid, 0, EVRNotificationType_Transient, "x", EVRNotificationStyle_Application, nullptr, &id);
	r["invalid_overlay_id"] = id;
	id = 9;
	r["unknown_overlay"] = (int)notes->CreateNotification(0xDEADBEEF, 0, EVRNotificationType_Transient, "x", EVRNotificationStyle_Application, nullptr, &id);
	r["unknown_overlay_id"] = id;
	c.Record("overlay refused", r);

	r = Json::Value();
	VRNotificationId user = 0, user2 = 0;
	r["user_value"] = (int)notes->CreateNotification(oh, 77, EVRNotificationType_Transient_SystemWithUserValue, "user", EVRNotificationStyle_Application, nullptr, &user);
	r["same_user_value"] = (int)notes->CreateNotification(oh, 77, EVRNotificationType_Transient_SystemWithUserValue, "user", EVRNotificationStyle_Application, nullptr, &user2);
	r["same_user_value_id"] = user2;
	r["remove_user"] = (int)notes->RemoveNotification(user);
	r["user_value_again"] = (int)notes->CreateNotification(oh, 77, EVRNotificationType_Transient_SystemWithUserValue, "user", EVRNotificationStyle_Application, nullptr, &user2);
	notes->RemoveNotification(user2);
	c.Record("user value", r);

	r = Json::Value();
	r["remove_first"] = (int)notes->RemoveNotification(first);
	r["remove_first_again"] = (int)notes->RemoveNotification(first);
	r["remove_never_made"] = (int)notes->RemoveNotification(123456);
	r["remove_zero"] = (int)notes->RemoveNotification(0);
	r["remove_second"] = (int)notes->RemoveNotification(second);
	notes->RemoveNotification(bitmapped);

	// What the overlay is told: nothing is queued for it by creating or removing a notification
	int events = 0;
	vr::VREvent_t ev;
	while (ov->PollNextOverlayEvent(oh, &ev, sizeof(ev)) && events < 100)
		events++;
	r["overlay_events"] = events;
	c.Record("remove", r);

	ov->DestroyOverlay(oh);
	c.core->Cleanup();
}

// The interfaces that keep state, across a Cleanup and Init: an anchor's descriptor, the driver list, the debug
// requests and SteamVR's resource files (the fixture of tests/unit/data/steamvr, OC_TEST_STEAMVR_DIR)
void StateAndResourcesCase(CaseContext& c)
{
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("Init failed");
		return;
	}
	vr::EVRInitError le = vr::VRInitError_None;
	auto* anchors = (vr::IVRSpatialAnchors_001::IVRSpatialAnchors*)c.core->GetGenericInterface("IVRSpatialAnchors_001", &le);
	auto* drivers = (vr::IVRDriverManager_001::IVRDriverManager*)c.core->GetGenericInterface("IVRDriverManager_001", &le);
	auto* debug = (vr::IVRDebug_001::IVRDebug*)c.core->GetGenericInterface("IVRDebug_001", &le);
	auto* resources = (vr::IVRResources_001::IVRResources*)c.core->GetGenericInterface("IVRResources_001", &le);
	if (!anchors || !drivers || !debug || !resources) {
		c.Record("an interface is missing", Err(le));
		c.core->Cleanup();
		return;
	}

	// An anchor's descriptor made before a Cleanup, used after the next Init
	vr::IVRSpatialAnchors_001::SpatialAnchorPose_t pose = { { { { 1, 0, 0, 0.5f }, { 0, 1, 0, 1.25f }, { 0, 0, 1, -2 } } } };
	vr::SpatialAnchorHandle_t handle = 0;
	Json::Value r;
	r["create"] = (int)anchors->CreateSpatialAnchorFromPose(0, vr::TrackingUniverseStanding, &pose, &handle);
	uint32_t length = 0;
	anchors->GetSpatialAnchorDescriptor(handle, nullptr, &length);
	std::string descriptor(length, '\0');
	r["descriptor"] = (int)anchors->GetSpatialAnchorDescriptor(handle, descriptor.data(), &length);
	r["descriptor_not_empty"] = !descriptor.empty() && descriptor[0] != '\0';
	char tiny[2] = {};
	uint32_t tinyLength = sizeof(tiny);
	r["descriptor_small_buffer"] = (int)anchors->GetSpatialAnchorDescriptor(handle, tiny, &tinyLength);
	r["descriptor_small_length_is_full"] = tinyLength == length;
	c.Record("anchor made", r);

	char name[64] = {};
	drivers->GetDriverName(0, name, sizeof(name));
	const uint32_t driverCount = drivers->GetDriverCount();
	c.core->Cleanup();

	r = Json::Value();
	if (c.core->Init(vr::VRApplication_Scene, nullptr) != vr::VRInitError_None) {
		c.Record("second Init failed");
		return;
	}
	anchors = (vr::IVRSpatialAnchors_001::IVRSpatialAnchors*)c.core->GetGenericInterface("IVRSpatialAnchors_001", &le);
	drivers = (vr::IVRDriverManager_001::IVRDriverManager*)c.core->GetGenericInterface("IVRDriverManager_001", &le);
	debug = (vr::IVRDebug_001::IVRDebug*)c.core->GetGenericInterface("IVRDebug_001", &le);
	resources = (vr::IVRResources_001::IVRResources*)c.core->GetGenericInterface("IVRResources_001", &le);
	if (!anchors || !drivers || !debug || !resources) {
		c.Record("an interface is missing after Init", Err(le));
		c.core->Cleanup();
		return;
	}
	vr::SpatialAnchorHandle_t restored = 0;
	r["from_descriptor"] = (int)anchors->CreateSpatialAnchorFromDescriptor(descriptor.c_str(), &restored);
	vr::IVRSpatialAnchors_001::SpatialAnchorPose_t back = {};
	r["pose_ret"] = (int)anchors->GetSpatialAnchorPose(restored, vr::TrackingUniverseStanding, &back);
	r["same_pose"] = memcmp(&back, &pose, sizeof(pose)) == 0;
	r["old_handle_ret"] = (int)anchors->GetSpatialAnchorPose(handle, vr::TrackingUniverseStanding, &back);
	r["junk_descriptor"] = (int)anchors->CreateSpatialAnchorFromDescriptor("not a descriptor", &restored);
	c.Record("anchor after Init", r);

	r = Json::Value();
	char again[64] = {};
	drivers->GetDriverName(0, again, sizeof(again));
	r["same_count"] = drivers->GetDriverCount() == driverCount;
	r["same_name"] = std::string(name) == again;
	r["handle_of_name"] = drivers->GetDriverHandle(name) != 0;
	r["unknown_name_handle"] = (Json::UInt64)drivers->GetDriverHandle("no_such_driver");
	r["enabled"] = drivers->IsEnabled(0);
	r["unknown_enabled"] = drivers->IsEnabled(999);
	r["unknown_name_size"] = drivers->GetDriverName(999, again, sizeof(again));
	c.Record("drivers after Init", r);

	r = Json::Value();
	char response[64] = {};
	r["emit"] = (int)debug->EmitVrProfilerEvent("oc event");
	vr::IVRDebug_001::VrProfilerEventHandle_t event = 0;
	r["begin"] = (int)debug->BeginVrProfilerEvent(&event);
	r["finish"] = (int)debug->FinishVrProfilerEvent(event, "oc event");
	r["request_size"] = debug->DriverDebugRequest(vr::k_unTrackedDeviceIndex_Hmd, "oc_unknown_request", response, sizeof(response));
	r["request_response"] = response;
	r["request_no_device"] = debug->DriverDebugRequest(999, "oc_unknown_request", response, sizeof(response));
	c.Record("debug", r);

	// SteamVR's files: a model's description as a shared resource, and the full path of resources that are and aren't there
	r = Json::Value();
	char path[1024] = {};
	const char* file = "test_controller/test_controller.json";
	uint32_t size = resources->GetResourceFullPath(file, "rendermodels", path, sizeof(path));
	r["path_size_matches"] = size == strlen(path) + 1;
	std::string full = path;
	for (char& ch : full)
		if (ch == '\\')
			ch = '/';
	r["path_ends_with_file"] = full.size() > strlen(file) && full.compare(full.size() - strlen(file), strlen(file), file) == 0;
	r["path_in_resources"] = full.find("/resources/rendermodels/") != std::string::npos;
	r["missing_file_size"] = resources->GetResourceFullPath("no_such_file.json", "rendermodels", path, sizeof(path));
	r["outside_size"] = resources->GetResourceFullPath("../../outside", "rendermodels", path, sizeof(path));
	r["small_buffer_size_is_full"] = resources->GetResourceFullPath(file, "rendermodels", path, 4) == size;
	c.Record("resource paths", r);

	r = Json::Value();
	const std::string shared = std::string("rendermodels/") + file;
	std::vector<char> text(65536);
	uint32_t loaded = resources->LoadSharedResource(shared.c_str(), text.data(), (uint32_t)text.size());
	r["size_set"] = loaded > 0;
	r["text_starts_with_brace"] = loaded > 0 && text[0] == '{';
	r["size_only"] = resources->LoadSharedResource(shared.c_str(), nullptr, 0) == loaded;
	r["small_buffer_size_is_full"] = resources->LoadSharedResource(shared.c_str(), text.data(), 4) == loaded;
	r["missing"] = resources->LoadSharedResource("rendermodels/no_such/none.json", text.data(), (uint32_t)text.size());
	r["outside"] = resources->LoadSharedResource("../outside", text.data(), (uint32_t)text.size());
	c.Record("shared resources", r);

	c.core->Cleanup();
}

} // namespace

const std::map<std::string, CaseFn>& conformance::CoverageCases()
{
	static const std::map<std::string, CaseFn> cases = {
		{ "client-core-methods", ClientCoreMethodsCase },
		{ "c-api-exports", CApiExportsCase },
		{ "init-held", InitHeldCase },
		{ "app-type-interfaces", AppTypeInterfacesCase },
		{ "mailbox", MailboxCase },
		{ "notifications", NotificationsCase },
		{ "state-and-resources", StateAndResourcesCase },
	};
	return cases;
}
