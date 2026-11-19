// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Input/Bindings/Evaluator.h"
#include "Misc/Input/Bindings/LegacyBindings.h"

#include <filesystem>

namespace {
const std::filesystem::path kFakeSteamVR = std::filesystem::path(OC_TEST_DATA_DIR) / "legacy_steamvr";

// Doubles, so the tests' whole numbers (0, 1) need no conversion warning
RawInputState Raw(std::map<std::string, double> values, double time = 0)
{
	RawInputState in;
	in.time = time;
	for (auto& [k, v] : values)
		in.value[k] = static_cast<float>(v);
	return in;
}

// The legacy file evaluated for one frame, and one hand's controller state from it
vr::VRControllerState_t Evaluate(BindingEvaluator& e, const RawInputState& in, const std::string& hand)
{
	e.Update(in);
	vr::VRControllerState_t state{};
	ApplyLegacyOutputs(e.Outputs(), hand, state);
	return state;
}

constexpr uint64_t Bit(vr::EVRButtonId id) { return 1ull << id; }
} // namespace

TEST_CASE("Legacy_index_trackpad_axis")
{
	BindingFile f = LoadLegacyBindingsFrom("knuckles", kFakeSteamVR);
	CHECK(f.controllerType == "knuckles");
	BindingEvaluator e(f);
	e.SetActiveActionSets({ { kLegacyActionSet, 0 } });
	vr::VRControllerState_t s = Evaluate(e, Raw({ { "/user/hand/right/input/trackpad/x", 0.25 }, { "/user/hand/right/input/trackpad/y", -0.5 },
	                                                { "/user/hand/right/input/trackpad/touch", 1 }, { "/user/hand/right/input/trigger/value", 0.75 } }),
	    "right");
	// The fixture puts the trackpad on axis 3
	CHECK(s.rAxis[3].x == doctest::Approx(0.25));
	CHECK(s.rAxis[3].y == doctest::Approx(-0.5));
	CHECK(s.rAxis[0].x == 0);
	CHECK((s.ulButtonTouched & Bit(vr::k_EButton_Axis3)) != 0);
	CHECK(s.rAxis[1].x == doctest::Approx(0.75));
	CHECK(s.rAxis[1].y == 0);
	// The left hand's state gets none of it
	vr::VRControllerState_t left{};
	ApplyLegacyOutputs(e.Outputs(), "left", left);
	CHECK(left.rAxis[3].x == 0);
	CHECK(left.ulButtonTouched == 0);
	// The file's axis types and buttons
	CHECK(LegacyAxisType(f, "right", 3) == vr::k_eControllerAxis_TrackPad);
	CHECK(LegacyAxisType(f, "right", 1) == vr::k_eControllerAxis_Trigger);
	CHECK(LegacyAxisType(f, "right", 0) == vr::k_eControllerAxis_None);
	CHECK(LegacySupportedButtons(f, "right") == (Bit(vr::k_EButton_Axis3) | Bit(vr::k_EButton_System)));
}

TEST_CASE("Legacy_system_button")
{
	BindingEvaluator e(LoadLegacyBindingsFrom("knuckles", kFakeSteamVR));
	e.SetActiveActionSets({ { kLegacyActionSet, 0 } });
	vr::VRControllerState_t s = Evaluate(e, Raw({ { "/user/hand/left/input/system/click", 1 } }), "left");
	CHECK(s.ulButtonPressed == Bit(vr::k_EButton_System));
	s = Evaluate(e, Raw({ { "/user/hand/left/input/system/click", 0 } }, 0.1), "left");
	CHECK(s.ulButtonPressed == 0);
}

TEST_CASE("Legacy_touch_a_b")
{
	BindingFile f = LoadLegacyBindingsFrom("oculus_touch", kFakeSteamVR);
	BindingEvaluator e(f);
	e.SetActiveActionSets({ { kLegacyActionSet, 0 } });
	vr::VRControllerState_t right = Evaluate(e, Raw({ { "/user/hand/right/input/a/click", 1 }, { "/user/hand/right/input/b/click", 1 } }), "right");
	CHECK(right.ulButtonPressed == (Bit(vr::k_EButton_A) | Bit(vr::k_EButton_ApplicationMenu)));
	CHECK(right.ulButtonTouched == Bit(vr::k_EButton_A)); // a pressed is touched; b has no touch output
	vr::VRControllerState_t left = Evaluate(e, Raw({ { "/user/hand/left/input/x/click", 1 } }, 0.1), "left");
	CHECK(left.ulButtonPressed == Bit(vr::k_EButton_A));
}

TEST_CASE("Legacy_fallback_without_steamvr")
{
	// No SteamVR: OpenComposite's own file, mapping as SteamVR's does
	for (const std::optional<std::filesystem::path>& dir : { std::optional<std::filesystem::path>(), std::optional(kFakeSteamVR / "missing") }) {
		BindingFile f = LoadLegacyBindingsFrom("knuckles", dir);
		CHECK(f.controllerType == "knuckles");
		CHECK(f.warnings.empty());
		BindingEvaluator e(f);
		e.SetActiveActionSets({ { kLegacyActionSet, 0 } });
		vr::VRControllerState_t s = Evaluate(e, Raw({ { "/user/hand/right/input/trigger/value", 0.9 }, { "/user/hand/right/input/a/click", 1 } }), "right");
		CHECK(s.rAxis[1].x == doctest::Approx(0.9));
		CHECK((s.ulButtonPressed & Bit(vr::k_EButton_Axis1)) != 0); // over the button binding's 0.55
		CHECK((s.ulButtonPressed & Bit(vr::k_EButton_Grip)) != 0); // Index's A is the old grip button, as SteamVR maps it
		CHECK(LegacyAxisType(f, "right", 0) == vr::k_eControllerAxis_TrackPad);
	}
	// A type with no file of its own gets the generic one, from SteamVR's folder or built in
	CHECK(LegacyBindingsRelativePath("hpmotioncontroller") == LegacyBindingsRelativePath("generic"));
	BindingFile generic = LoadLegacyBindingsFrom("hpmotioncontroller", std::nullopt);
	CHECK(generic.sets.count(kLegacyActionSet) == 1);
	CHECK(LegacyAxisType(generic, "left", 2) == vr::k_eControllerAxis_Joystick);
	// Every built-in file reads without warnings
	for (const char* type : { "knuckles", "oculus_touch", "vive_controller", "generic" }) {
		BindingFile b = BuiltInLegacyBindings(type);
		CHECK(b.warnings.empty());
		BindingEvaluator check(b);
		CHECK(check.Warnings().empty());
	}
}

TEST_CASE("Legacy_thumbstick_push_presses_axis0")
{
	// Index's legacy file: a button on the thumbstick with force_input position presses axis 0 when the stick is
	// pushed past 0.8, and lets go under 0.7
	BindingEvaluator e(BuiltInLegacyBindings("knuckles"));
	e.SetActiveActionSets({ { kLegacyActionSet, 0 } });
	auto pressed = [&](double x, double t) {
		vr::VRControllerState_t s = Evaluate(e, Raw({ { "/user/hand/left/input/thumbstick/x", x }, { "/user/hand/left/input/thumbstick/y", 0 } }, t), "left");
		return (s.ulButtonPressed & Bit(vr::k_EButton_Axis0)) != 0;
	};
	CHECK_FALSE(pressed(0, 0));
	CHECK_FALSE(pressed(0.5, 0.1));
	CHECK(pressed(-0.9, 0.2));
	CHECK(pressed(0.75, 0.3));
	CHECK_FALSE(pressed(0.6, 0.4));
}

TEST_CASE("Legacy_state_changes_ignore_the_packet_number")
{
	vr::VRControllerState_t a{}, b{};
	a.unPacketNum = 3;
	b.unPacketNum = 9;
	CHECK_FALSE(LegacyStateChanged(a, b));
	b.rAxis[4].y = 0.1f;
	CHECK(LegacyStateChanged(a, b));
	b = a;
	b.ulButtonTouched = 1;
	CHECK(LegacyStateChanged(a, b));
}

TEST_CASE("Legacy_finger_curls_fill_only_the_free_axes")
{
	const float curl[5] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f };
	vr::VRControllerState_t s{};
	s.rAxis[3].x = 7;
	ApplyLegacyFingerCurls(curl, false, true, s);
	CHECK(s.rAxis[3].x == 7);
	CHECK(s.rAxis[4].x == doctest::Approx(0.4f * 1.66f));
	CHECK(s.rAxis[4].y == doctest::Approx(0.5f * 1.66f));
	ApplyLegacyFingerCurls(curl, true, false, s);
	CHECK(s.rAxis[3].x == doctest::Approx(0.2f * 1.66f * 1.33f));
	CHECK(s.rAxis[3].y == doctest::Approx(0.3f * 1.66f));

	// A closed fist reads 1, not past it
	const float fist[5] = { 1, 1, 0.9f, 1, 1 };
	ApplyLegacyFingerCurls(fist, true, true, s);
	CHECK(s.rAxis[3].x == 1.0f);
	CHECK(s.rAxis[3].y == 1.0f);
	CHECK(s.rAxis[4].x == 1.0f);
	CHECK(s.rAxis[4].y == 1.0f);
}

TEST_CASE("Legacy_haptic_strength_follows_the_duration")
{
	CHECK(LegacyHapticStrength(3999) == doctest::Approx(1.0f));
	CHECK(LegacyHapticStrength(5000) == doctest::Approx(1.0f));
	CHECK(LegacyHapticStrength(100) == doctest::Approx(100.0f / 3999.0f));
	CHECK(LegacyHapticStrength(0) == 0.0f);
}
