// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Drivers/Backend.h"
#include "Misc/Process.h"
#include "Misc/Dashboard.h"
#include "Misc/SharedStore.h"
#include "Reimpl/BaseOverlay.h"

#include <limits>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace vr;

namespace {

VROverlayHandle_t Make(BaseOverlay& overlays, const char* key)
{
	VROverlayHandle_t h = k_ulOverlayHandleInvalid;
	REQUIRE(overlays.CreateOverlay(key, "Test", &h) == VROverlayError_None);
	REQUIRE(h != k_ulOverlayHandleInvalid);
	return h;
}

} // namespace

TEST_CASE("A_new_overlay_is_white_opaque_and_unrestricted")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.defaults");

	float r = -1, g = -1, b = -1, a = -1;
	CHECK(overlays.GetOverlayColor(h, &r, &g, &b) == VROverlayError_None);
	CHECK(overlays.GetOverlayAlpha(h, &a) == VROverlayError_None);
	CHECK(r == 1.0f);
	CHECK(g == 1.0f);
	CHECK(b == 1.0f);
	CHECK(a == 1.0f);

	float rangeMin = -1, rangeMax = -1;
	CHECK(overlays.GetOverlayAutoCurveDistanceRangeInMeters(h, &rangeMin, &rangeMax) == VROverlayError_None);
	CHECK(rangeMin == 0.0f);
	CHECK(rangeMax == 0.0f);

	// Its creator may render to it
	CHECK(overlays.GetOverlayRenderingPid(h) == oc_current_pid());

	// Hidden until shown
	CHECK_FALSE(overlays.IsOverlayVisible(h));
}

TEST_CASE("Overlay_getters_refuse_null_pointers")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.null");
	float f = 0;
	CHECK(overlays.GetOverlayColor(h, nullptr, &f, &f) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayAlpha(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayCurvature(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayAutoCurveDistanceRangeInMeters(h, &f, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayWidthInMeters(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlaySortOrder(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTextureColorSpace(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTextureBounds(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTransformType(h, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayFlag(h, (OOVR_VROverlayFlags)3, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.SetOverlayTransformAbsolute(h, TrackingUniverseStanding, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayTransformAbsolute(h, nullptr, nullptr) == VROverlayError_InvalidParameter);
	CHECK(overlays.SetOverlayTransformTrackedDeviceRelative(h, 0, nullptr) == VROverlayError_InvalidParameter);
}

TEST_CASE("A_flag_number_without_a_bit_is_refused")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.flagrange");
	bool on = true;
	CHECK(overlays.SetOverlayFlag(h, (OOVR_VROverlayFlags)67, true) == VROverlayError_InvalidParameter);
	CHECK(overlays.SetOverlayFlag(h, (OOVR_VROverlayFlags)-1, true) == VROverlayError_InvalidParameter);
	CHECK(overlays.GetOverlayFlag(h, (OOVR_VROverlayFlags)67, &on) == VROverlayError_InvalidParameter);
	// A handle that's no overlay is answered as such first
	CHECK(overlays.SetOverlayFlag((VROverlayHandle_t)0x1234, (OOVR_VROverlayFlags)67, true) == VROverlayError_InvalidHandle);
	CHECK(overlays.GetOverlayFlag((VROverlayHandle_t)0x1234, (OOVR_VROverlayFlags)67, &on) == VROverlayError_InvalidHandle);
	// Bit 3 (67 taken modulo 64) is untouched
	uint32_t all = 0xffffffff;
	CHECK(overlays.GetOverlayFlags(h, &all) == VROverlayError_None);
	CHECK(all == 0);
	CHECK(overlays.SetOverlayFlag(h, (OOVR_VROverlayFlags)63, true) == VROverlayError_None);
	CHECK(overlays.GetOverlayFlag(h, (OOVR_VROverlayFlags)63, &on) == VROverlayError_None);
	CHECK(on);
}

TEST_CASE("An_absolute_transform_never_set_still_answers_an_origin")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.origin");
	ETrackingUniverseOrigin origin = (ETrackingUniverseOrigin)99;
	HmdMatrix34_t m{};
	CHECK(overlays.GetOverlayTransformAbsolute(h, &origin, &m) == VROverlayError_None);
	CHECK((origin == TrackingUniverseSeated || origin == TrackingUniverseStanding));
}

TEST_CASE("Finding_an_overlay_that_does_not_exist_says_so")
{
	BaseOverlay overlays;
	VROverlayHandle_t h = 7;
	CHECK(overlays.FindOverlay("oc.test.nobody", &h) == VROverlayError_UnknownOverlay);
	CHECK(h == k_ulOverlayHandleInvalid);
}

TEST_CASE("At_most_128_overlays_exist")
{
	BaseOverlay overlays;
	VROverlayHandle_t h;
	for (int i = 0; i < 128; i++)
		CHECK(overlays.CreateOverlay(("oc.test.many." + std::to_string(i)).c_str(), "Test", &h) == VROverlayError_None);
	CHECK(overlays.CreateOverlay("oc.test.many.one.too.many", "Test", &h) == VROverlayError_OverlayLimitExceeded);
	CHECK(h == k_ulOverlayHandleInvalid);
	// A destroyed overlay makes room
	REQUIRE(overlays.FindOverlay("oc.test.many.0", &h) == VROverlayError_None);
	CHECK(overlays.DestroyOverlay(h) == VROverlayError_None);
	CHECK(overlays.CreateOverlay("oc.test.many.again", "Test", &h) == VROverlayError_None);
}

TEST_CASE("Overlay_relative_transform_is_only_read_back_when_set")
{
	BaseOverlay overlays;
	const VROverlayHandle_t child = Make(overlays, "oc.test.child");
	const VROverlayHandle_t parent = Make(overlays, "oc.test.parent");
	VROverlayHandle_t got = 5;
	HmdMatrix34_t m{};
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_WrongTransformType);
	CHECK(overlays.SetOverlayTransformOverlayRelative(child, parent, &m) == VROverlayError_None);
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_None);
	CHECK(got == parent);

	// Destroying the parent leaves nothing pointing at it
	CHECK(overlays.DestroyOverlay(parent) == VROverlayError_None);
	CHECK(overlays.GetOverlayTransformOverlayRelative(child, &got, &m) == VROverlayError_None);
	CHECK(got == k_ulOverlayHandleInvalid);
}

TEST_CASE("Any_overlay_event_is_polled_with_its_overlay_handle")
{
	BaseOverlay overlays;
	const VROverlayHandle_t first = Make(overlays, "oc.test.pollany.first");
	const VROverlayHandle_t second = Make(overlays, "oc.test.pollany.second");
	VREvent_t e{};
	VROverlayHandle_t from = k_ulOverlayHandleInvalid;
	CHECK_FALSE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));

	REQUIRE(overlays.ShowOverlay(second) == VROverlayError_None);
	REQUIRE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));
	CHECK(from == second);
	CHECK(e.eventType == VREvent_OverlayShown);
	CHECK_FALSE(overlays.PollNextAnyOverlayEvent(&e, sizeof(e), &from));
	(void)first;
}

TEST_CASE("An_overlay_of_no_area_is_not_hit")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.area");
	HmdMatrix34_t ahead{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, -1 } } };
	REQUIRE(overlays.SetOverlayTransformAbsolute(h, TrackingUniverseStanding, &ahead) == VROverlayError_None);
	// Curved, without a runtime to say whether it can be: placed flat
	CHECK(overlays.SetOverlayCurvature(h, 0.5f) == VROverlayError_None);
	OOVR_VROverlayIntersectionParams_t params{};
	params.vSource = { 0, 0, 0 };
	params.vDirection = { 0, 0, -1 };
	params.eOrigin = TrackingUniverseStanding;
	OOVR_VROverlayIntersectionResults_t results{};
	CHECK(overlays.ComputeOverlayIntersection(h, &params, &results));
	CHECK(results.fDistance == doctest::Approx(1.0f));
	for (float width : { 0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN() }) {
		CHECK(overlays.SetOverlayWidthInMeters(h, width) == VROverlayError_None);
		CHECK_FALSE(overlays.ComputeOverlayIntersection(h, &params, &results));
	}
}

TEST_CASE("Polling_an_overlay_event_into_null_answers_none")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.pollnull");
	REQUIRE(overlays.ShowOverlay(h) == VROverlayError_None);
	CHECK_FALSE(overlays.PollNextOverlayEvent(h, nullptr, sizeof(VREvent_t)));
	// The event is still there
	VREvent_t e{};
	REQUIRE(overlays.PollNextOverlayEvent(h, &e, sizeof(e)));
	CHECK(e.eventType == VREvent_OverlayShown);
}

TEST_CASE("An_intersection_mask_has_at_most_32_primitives_of_known_types")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.mask");
	std::vector<OOVR_VROverlayIntersectionMaskPrimitive_t> prims(33);
	for (auto& p : prims) {
		p = {};
		p.m_nPrimitiveType = OverlayIntersectionPrimitiveType_Circle;
		p.m_Primitive.m_Circle = { 0.5f, 0.5f, 0.25f };
	}
	const uint32_t size = sizeof(OOVR_VROverlayIntersectionMaskPrimitive_t);
	CHECK(overlays.SetOverlayIntersectionMask(h, prims.data(), 32, size) == VROverlayError_None);
	CHECK(overlays.SetOverlayIntersectionMask(h, prims.data(), 33, size) == VROverlayError_TooManyMaskPrimitives);
	CHECK(overlays.SetOverlayIntersectionMask(h, prims.data(), 1, size + 4) == VROverlayError_BadMaskPrimitive);
	prims[0].m_nPrimitiveType = (OOVR_EVROverlayIntersectionMaskPrimitiveType)7;
	CHECK(overlays.SetOverlayIntersectionMask(h, prims.data(), 1, size) == VROverlayError_BadMaskPrimitive);
}

TEST_CASE("Neighbours_link_both_ways_and_clear_both_ends")
{
	BaseOverlay overlays;
	const VROverlayHandle_t a = Make(overlays, "oc.test.nb.a");
	const VROverlayHandle_t b = Make(overlays, "oc.test.nb.b");
	CHECK(overlays.SetOverlayNeighbor(OverlayDirection_Right, a, b) == VROverlayError_None);
	CHECK(overlays.MoveGamepadFocusToNeighbor(OverlayDirection_Left, b) == VROverlayError_None);
	CHECK(overlays.GetGamepadFocusOverlay() == a);
	CHECK(overlays.MoveGamepadFocusToNeighbor(OverlayDirection_Right, a) == VROverlayError_None);
	CHECK(overlays.GetGamepadFocusOverlay() == b);

	CHECK(overlays.SetOverlayNeighbor(OverlayDirection_Right, a, k_ulOverlayHandleInvalid) == VROverlayError_None);
	CHECK(overlays.MoveGamepadFocusToNeighbor(OverlayDirection_Right, a) == VROverlayError_NoNeighbor);
	CHECK(overlays.MoveGamepadFocusToNeighbor(OverlayDirection_Left, b) == VROverlayError_NoNeighbor);

	CHECK(overlays.SetOverlayNeighbor(OverlayDirection_Up, a, (VROverlayHandle_t)0x1234) == VROverlayError_InvalidHandle);
	CHECK(overlays.SetOverlayNeighbor(OverlayDirection_Up, (VROverlayHandle_t)0x1234, b) == VROverlayError_InvalidHandle);
}

TEST_CASE("Moving_gamepad_focus_tells_both_overlays")
{
	BaseOverlay overlays;
	const VROverlayHandle_t a = Make(overlays, "oc.test.focus.a");
	const VROverlayHandle_t b = Make(overlays, "oc.test.focus.b");
	VREvent_t e{};
	CHECK(overlays.SetGamepadFocusOverlay(a) == VROverlayError_None);
	REQUIRE(overlays.PollNextOverlayEvent(a, &e, sizeof(e)));
	CHECK(e.eventType == VREvent_OverlayGamepadFocusGained);
	CHECK(overlays.SetGamepadFocusOverlay(b) == VROverlayError_None);
	REQUIRE(overlays.PollNextOverlayEvent(a, &e, sizeof(e)));
	CHECK(e.eventType == VREvent_OverlayGamepadFocusLost);
	REQUIRE(overlays.PollNextOverlayEvent(b, &e, sizeof(e)));
	CHECK(e.eventType == VREvent_OverlayGamepadFocusGained);
	CHECK(overlays.SetGamepadFocusOverlay((VROverlayHandle_t)0x1234) == VROverlayError_InvalidHandle);
	CHECK(overlays.GetGamepadFocusOverlay() == b);
	// Destroying the focused overlay leaves none
	CHECK(overlays.DestroyOverlay(b) == VROverlayError_None);
	CHECK(overlays.GetGamepadFocusOverlay() == k_ulOverlayHandleInvalid);
}

TEST_CASE("A_destroyed_overlay_s_handle_stays_invalid")
{
	BaseOverlay overlays;
	std::vector<VROverlayHandle_t> old;
	for (int i = 0; i < 50; i++) {
		// The new overlay may well have the memory the destroyed one had: its handle is still another
		const VROverlayHandle_t h = Make(overlays, "oc.test.again");
		for (VROverlayHandle_t o : old) {
			CHECK(o != h);
			float alpha = 0;
			CHECK(overlays.GetOverlayAlpha(o, &alpha) == VROverlayError_InvalidHandle);
		}
		CHECK(overlays.DestroyOverlay(h) == VROverlayError_None);
		old.push_back(h);
	}
	// A made-up value is no overlay either
	float alpha = 0;
	CHECK(overlays.GetOverlayAlpha(0x12345678, &alpha) == VROverlayError_InvalidHandle);
}

TEST_CASE("A_child_of_a_hidden_parent_is_not_visible")
{
	BaseOverlay overlays;
	const VROverlayHandle_t parent = Make(overlays, "oc.test.parent");
	const VROverlayHandle_t child = Make(overlays, "oc.test.child");
	const VROverlayHandle_t grandchild = Make(overlays, "oc.test.grandchild");
	HmdMatrix34_t identity{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } } };
	REQUIRE(overlays.SetOverlayTransformOverlayRelative(child, parent, &identity) == VROverlayError_None);
	REQUIRE(overlays.SetOverlayTransformOverlayRelative(grandchild, child, &identity) == VROverlayError_None);
	overlays.ShowOverlay(child);
	overlays.ShowOverlay(grandchild);
	CHECK_FALSE(overlays.IsOverlayVisible(child));
	CHECK_FALSE(overlays.IsOverlayVisible(grandchild));

	overlays.ShowOverlay(parent);
	CHECK(overlays.IsOverlayVisible(child));
	CHECK(overlays.IsOverlayVisible(grandchild));

	overlays.HideOverlay(child);
	CHECK(overlays.IsOverlayVisible(parent));
	CHECK_FALSE(overlays.IsOverlayVisible(grandchild));

	// A parent that's gone: placed by nothing, and seen as its own setting says
	overlays.ShowOverlay(child);
	CHECK(overlays.DestroyOverlay(parent) == VROverlayError_None);
	CHECK(overlays.IsOverlayVisible(child));
}

TEST_CASE("A_dashboard_overlay_goes_with_its_other_half")
{
	BaseOverlay overlays;
	VROverlayHandle_t main = k_ulOverlayHandleInvalid, thumbnail = k_ulOverlayHandleInvalid;
	if (overlays.CreateDashboardOverlay("oc.test.dash", "Dash", &main, &thumbnail) != VROverlayError_None) {
		// Only where a game runs on this machine with its dashboardButton off, which has nowhere to show it
		REQUIRE_FALSE(SceneDocHasDashboard(SharedStore::Get().Read(StoreDoc::Scene), SharedStore::IsSameProcessAlive));
		return;
	}
	CHECK(overlays.DestroyOverlay(main) == VROverlayError_None);
	float alpha = 0;
	CHECK(overlays.GetOverlayAlpha(thumbnail, &alpha) == VROverlayError_InvalidHandle);

	REQUIRE(overlays.CreateDashboardOverlay("oc.test.dash", "Dash", &main, &thumbnail) == VROverlayError_None);
	CHECK(overlays.DestroyOverlay(thumbnail) == VROverlayError_None);
	CHECK(overlays.GetOverlayAlpha(main, &alpha) == VROverlayError_InvalidHandle);
	// Its key is free again
	REQUIRE(overlays.CreateDashboardOverlay("oc.test.dash", "Dash", &main, &thumbnail) == VROverlayError_None);
}

TEST_CASE("An_overlay_key_of_another_running_program_is_in_use")
{
	const uint32_t self = oc_current_pid();
#ifdef _WIN32
	const uint32_t other = 4; // System, always there
#else
	const uint32_t other = (uint32_t)getppid();
#endif
	Json::Value doc(Json::objectValue);
	Json::Value o(Json::objectValue);
	o["key"] = "oc.test.taken";
	o["handle"] = (Json::UInt64)1;
	doc[std::to_string(other)]["overlays"].append(o);
	CHECK(KeyOfAnotherProgram(doc, "oc.test.taken", self));
	CHECK_FALSE(KeyOfAnotherProgram(doc, "oc.test.free", self));
	// Its own overlays are the program's to answer for, and an ended program's keys are free
	CHECK_FALSE(KeyOfAnotherProgram(doc, "oc.test.taken", other));
	doc[std::to_string(self)]["overlays"].append(o);
	CHECK(KeyOfAnotherProgram(doc, "oc.test.taken", other));
	doc.removeMember(std::to_string(self));
	doc[std::to_string(other)]["start"] = (Json::UInt64)(SharedStore::ProcessStartTime(other) + 1);
	CHECK_FALSE(KeyOfAnotherProgram(doc, "oc.test.taken", self));
}

TEST_CASE("A_texture_not_copied_yet_has_the_size_it_says")
{
	VRVulkanTextureData_t data{};
	data.m_nWidth = 640;
	data.m_nHeight = 360;
	Texture_t vulkan{ &data, TextureType_Vulkan, ColorSpace_Auto };
	auto size = OverlayTextureSize(vulkan);
	REQUIRE(size);
	CHECK(size->first == 640);
	CHECK(size->second == 360);
	data.m_nWidth = 0;
	CHECK_FALSE(OverlayTextureSize(vulkan));
	Texture_t none{ nullptr, TextureType_Vulkan, ColorSpace_Auto };
	CHECK_FALSE(OverlayTextureSize(none));
	Texture_t gl{ (void*)(uintptr_t)1, TextureType_OpenGL, ColorSpace_Auto };
	CHECK_FALSE(OverlayTextureSize(gl));
}

TEST_CASE("Clearing_an_overlay_s_texture_needs_no_runtime")
{
	REQUIRE(BackendManager::InstancePtr() == nullptr);
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.clear");
	CHECK(overlays.ClearOverlayTexture(h) == VROverlayError_None);
	CHECK(overlays.ClearOverlayTexture((VROverlayHandle_t)0x1234) == VROverlayError_InvalidHandle);
}

TEST_CASE("A_texture_without_a_handle_is_refused")
{
	BaseOverlay overlays;
	const VROverlayHandle_t h = Make(overlays, "oc.test.nohandle");
	Texture_t none{ nullptr, TextureType_DirectX, ColorSpace_Auto };
	CHECK(overlays.SetOverlayTexture(h, &none) == VROverlayError_InvalidTexture);
	CHECK(overlays.SetOverlayTexture(h, nullptr) == VROverlayError_InvalidTexture);
}
