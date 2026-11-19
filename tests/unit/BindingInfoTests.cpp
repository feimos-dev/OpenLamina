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
