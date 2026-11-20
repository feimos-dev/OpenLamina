// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Misc/Input/Bindings/BindingInfo.h"

namespace {
BindingFile Index()
{
	return ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": {
		"sources": [
			{ "path": "/user/hand/right/input/trigger", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/Fire" } } },
			{ "path": "/user/hand/left/input/trigger", "mode": "trigger", "inputs": { "pull": { "output": "/actions/main/in/squeeze" } } },
			{ "path": "/user/hand/right/input/thumbstick", "mode": "dpad", "inputs": {
				"east": { "output": "/actions/main/in/tab_right" }, "west": { "output": "/actions/main/in/tab_left" } } },
			{ "path": "/user/hand/left/input/thumbstick", "mode": "joystick", "inputs": {
				"position": { "output": "/actions/main/in/move" }, "click": { "output": "/actions/main/in/run" } } },
			{ "path": "/user/hand/left/input/grip", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/grab" } } },
			{ "path": "/user/hand/right/input/grip", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/grab" } } },
			{ "path": "/user/hand/right/input/trackpad", "mode": "none", "inputs": { "click": { "output": "/actions/main/in/unused" } } },
			{ "path": "/user/hand/right/input/a", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/jump" } } } ],
		"chords": [ { "output": "/actions/main/in/both", "inputs": [ [ "/user/hand/left/input/a", "click" ], [ "/user/hand/right/input/b", "click" ] ] } ] } } })");
}
} // namespace

TEST_CASE("A_binding_is_told_with_its_mode_slot_and_what_the_user_moves")
{
	// A trigger bound as a button is still a trigger to the user, and the action is found whatever its case
	std::vector<ActionBinding> fire = BindingsForAction(Index(), "/actions/main/in/fire");
	REQUIRE(fire.size() == 1);
	CHECK(fire[0].device == "/user/hand/right");
	CHECK(fire[0].input == "/input/trigger");
	CHECK(fire[0].mode == "button");
	CHECK(fire[0].slot == "click");
	CHECK(fire[0].sourceType == "trigger");

	std::vector<ActionBinding> squeeze = BindingsForAction(Index(), "/actions/main/in/squeeze");
	REQUIRE(squeeze.size() == 1);
	CHECK(squeeze[0].mode == "trigger");
	CHECK(squeeze[0].slot == "pull");

	std::vector<ActionBinding> jump = BindingsForAction(Index(), "/actions/main/in/jump");
	REQUIRE(jump.size() == 1);
	CHECK(jump[0].input == "/input/a");
	CHECK(jump[0].sourceType == "button");
}

TEST_CASE("A_dpad_direction_is_a_binding")
{
	std::vector<ActionBinding> right = BindingsForAction(Index(), "/actions/main/in/tab_right");
	REQUIRE(right.size() == 1);
	CHECK(right[0].input == "/input/thumbstick");
	CHECK(right[0].mode == "dpad");
	CHECK(right[0].slot == "east");
	CHECK(right[0].sourceType == "joystick");
	CHECK(BindingsForAction(Index(), "/actions/main/in/tab_left")[0].slot == "west");
}

TEST_CASE("Each_input_of_a_source_is_its_own_binding")
{
	std::vector<ActionBinding> run = BindingsForAction(Index(), "/actions/main/in/run");
	REQUIRE(run.size() == 1);
	CHECK(run[0].mode == "joystick");
	CHECK(run[0].slot == "click");
	CHECK(BindingsForAction(Index(), "/actions/main/in/move")[0].slot == "position");
}

TEST_CASE("Bindings_on_both_hands_and_on_one")
{
	// The grip keeps SteamVR's name, and is squeezed on an Index controller
	std::vector<ActionBinding> grab = BindingsForAction(Index(), "/actions/main/in/grab");
	REQUIRE(grab.size() == 2);
	CHECK(grab[0].device == "/user/hand/left");
	CHECK(grab[1].device == "/user/hand/right");
	CHECK(grab[0].input == "/input/grip");
	CHECK(grab[0].sourceType == "trigger");

	std::vector<ActionBinding> left = BindingsForAction(Index(), "/actions/main/in/grab", "/user/hand/left");
	REQUIRE(left.size() == 1);
	CHECK(left[0].device == "/user/hand/left");
}

TEST_CASE("A_chord_binds_each_of_its_inputs")
{
	std::vector<ActionBinding> both = BindingsForAction(Index(), "/actions/main/in/both");
	REQUIRE(both.size() == 2);
	CHECK(both[0].mode == "chord");
	CHECK(both[0].input == "/input/a");
	CHECK(both[1].device == "/user/hand/right");
	CHECK(both[1].input == "/input/b");
}

TEST_CASE("Mode_none_and_an_unbound_action_bind_nothing")
{
	CHECK(BindingsForAction(Index(), "/actions/main/in/unused").empty());
	CHECK(BindingsForAction(Index(), "/actions/main/in/nothing").empty());
}

TEST_CASE("Index_fingers_are_triggers")
{
	BindingFile index = ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": { "sources": [
		{ "path": "/user/hand/left/input/finger/index", "mode": "trigger", "inputs": { "pull": { "output": "/actions/main/in/curl" } } },
		{ "path": "/user/hand/left/input/pinch", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/pinch" } } } ] } } })");
	std::vector<ActionBinding> curl = BindingsForAction(index, "/actions/main/in/curl");
	REQUIRE(curl.size() == 1);
	CHECK(curl[0].input == "/input/finger/index");
	CHECK(curl[0].sourceType == "trigger");
	CHECK(BindingsForAction(index, "/actions/main/in/pinch")[0].sourceType == "pinch");
}

TEST_CASE("A_grip_that_clicks_is_a_button")
{
	BindingFile vive = ParseBindingFile(R"({ "controller_type": "vive_controller", "bindings": { "/actions/main": { "sources": [
		{ "path": "/user/hand/left/input/grip", "mode": "button", "inputs": { "click": { "output": "/actions/main/in/grab" } } },
		{ "path": "/user/hand/left/input/trackpad", "mode": "trackpad", "inputs": { "position": { "output": "/actions/main/in/move" } } } ] } } })");
	CHECK(BindingsForAction(vive, "/actions/main/in/grab")[0].sourceType == "button");
	CHECK(BindingsForAction(vive, "/actions/main/in/move")[0].sourceType == "trackpad");
}

TEST_CASE("An_origin_names_the_render_model_component_of_its_input")
{
	CHECK(OriginComponentName("/user/hand/left/input/trigger") == "trigger");
	CHECK(OriginComponentName("/user/hand/right/input/thumbstick/click") == "thumbstick");
	CHECK(OriginComponentName("/user/hand/right").empty());
	CHECK(OriginFromInputPath("/user/hand/right/input/trigger/value") == "/user/hand/right/input/trigger");
	CHECK(OriginFromInputPath("/user/hand/right/output/haptic") == "/user/hand/right/output/haptic");
}

TEST_CASE("An_origin_is_named_from_the_pieces_asked_for")
{
	const std::string path = "/user/hand/right/input/trigger";
	CHECK(OriginLocalizedName(1, "oculus_touch", path, true, true, true) == "Right Hand Oculus Touch Trigger");
	CHECK(OriginLocalizedName(0, "knuckles", path, true, false, false) == "Left Hand");
	CHECK(OriginLocalizedName(0, "knuckles", path, false, true, true) == "Index Controller Trigger");
	CHECK(OriginLocalizedName(0, "", "/user/hand/left", true, true, true) == "Left Hand OpenXR Controller");
	CHECK(OriginLocalizedName(0, "oculus_touch", "/user/hand/left/input/a/click", false, false, true) == "A Button");
	CHECK(OriginLocalizedName(0, "oculus_touch", path, false, false, false).empty());
}

TEST_CASE("A_pose_is_bound_on_the_hands_the_file_names")
{
	BindingFile file = ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": {
		"poses": [ { "output": "/actions/main/in/Hand", "path": "/user/hand/right/pose/raw" } ],
		"skeleton": [ { "output": "/actions/main/in/skel", "path": "/user/hand/left/input/skeleton/left" } ] } } })");
	std::vector<std::string> pose = PoseDevicesForAction(file, "/actions/main/in/hand");
	REQUIRE(pose.size() == 1);
	CHECK(pose[0] == "/user/hand/right");
	std::vector<std::string> skeleton = PoseDevicesForAction(file, "/actions/main/in/skel");
	REQUIRE(skeleton.size() == 1);
	CHECK(skeleton[0] == "/user/hand/left");
	CHECK(PoseDevicesForAction(file, "/actions/main/in/none").empty());
}

TEST_CASE("A_force_sensor_bound_by_its_click_binds_nothing")
{
	BindingFile index = ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": { "sources": [
		{ "path": "/user/hand/left/input/grip", "mode": "force_sensor", "inputs": { "click": { "output": "/actions/main/in/grab" } } },
		{ "path": "/user/hand/right/input/grip", "mode": "force_sensor", "inputs": { "force": { "output": "/actions/main/in/squeeze" } } } ] } } })");
	CHECK(BindingsForAction(index, "/actions/main/in/grab").empty());
	REQUIRE(BindingsForAction(index, "/actions/main/in/squeeze").size() == 1);
	CHECK(BindingsForAction(index, "/actions/main/in/squeeze")[0].slot == "force");
}

TEST_CASE("Poses_skeletons_and_vibrations_are_told")
{
	BindingFile index = ParseBindingFile(R"({ "controller_type": "knuckles", "bindings": { "/actions/main": {
		"poses": [ { "output": "/actions/main/in/pose", "path": "/user/hand/left/pose/raw" },
			{ "output": "/actions/main/in/pose", "path": "/user/hand/right/pose/raw" } ],
		"skeleton": [ { "output": "/actions/main/in/skeleton_left", "path": "/user/hand/left/input/skeleton/left" } ],
		"haptics": [ { "output": "/actions/main/out/haptic", "path": "/user/hand/right/output/haptic" } ] } } })");
	std::vector<ActionBinding> pose = BindingsForAction(index, "/actions/main/in/pose");
	REQUIRE(pose.size() == 2);
	CHECK(pose[0].device == "/user/hand/left");
	CHECK(pose[0].input == "/pose/raw");
	CHECK(pose[0].mode.empty());
	CHECK(BindingsForAction(index, "/actions/main/in/pose", "/user/hand/right").size() == 1);
	std::vector<ActionBinding> skeleton = BindingsForAction(index, "/actions/main/in/skeleton_left");
	REQUIRE(skeleton.size() == 1);
	CHECK(skeleton[0].input == "/input/skeleton/left");
	// A vibration has no device or input path
	std::vector<ActionBinding> haptic = BindingsForAction(index, "/actions/main/out/haptic");
	REQUIRE(haptic.size() == 1);
	CHECK(haptic[0].device.empty());
	CHECK(haptic[0].input.empty());
	CHECK(BindingsForAction(index, "/actions/main/out/haptic", "/user/hand/left").empty());
}
