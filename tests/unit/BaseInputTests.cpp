// doctest first: stdafx.h brings in Xlib on Linux, whose macros (None, Bool, ...) clash with doctest's names
#include "doctest.h"

#include "stdafx.h"

#include "Reimpl/BaseInput.h"

#include <limits>

TEST_CASE("Mixed_priority_sets_without_second_set")
{
	// UpdateActionState's check, which it makes before anything else
	BaseInput::VRActiveActionSet_t sets[2] = {};
	sets[0].ulActionSet = 0xabcd0001; // as GetActionSetHandle hands out
	sets[0].nPriority = 1;
	// The second entry names no set, and has another priority
	sets[1].ulActionSet = vr::k_ulInvalidActionSetHandle;
	sets[1].nPriority = 0;
	CHECK(BaseInput::CheckActiveSets(sets, sizeof(sets[0]), 2) == vr::VRInputError_InvalidParam);

	// One set alone, or two that both name one, pass the check
	CHECK(BaseInput::CheckActiveSets(sets, sizeof(sets[0]), 1) == vr::VRInputError_None);
	sets[1].ulActionSet = 0xabcd0002;
	CHECK(BaseInput::CheckActiveSets(sets, sizeof(sets[0]), 2) == vr::VRInputError_None);
}

TEST_CASE("Never_changed_action_update_time")
{
	// Right after the action was made (a hundredth of a second ago): when it was made, seconds from now
	const double now = 5000.0;
	float t = BaseInput::UpdateTimeFromNow(0, now - 0.01, now);
	CHECK(t >= -1.0f);
	CHECK(t <= 0.0f);
	CHECK(t == doctest::Approx(-0.01f).epsilon(0.01));
	// Changed since: from the change
	CHECK(BaseInput::UpdateTimeFromNow(now - 0.5, now - 2, now) == doctest::Approx(-0.5f));
	// Never later than now
	CHECK(BaseInput::UpdateTimeFromNow(now + 1, now - 2, now) == 0.0f);
}

TEST_CASE("Haptic_duration_in_64_bits")
{
	// Five seconds is 5e9 nanoseconds, past what 32 bits hold
	CHECK(BaseInput::HapticDuration(5.0f) == 5000000000LL);
	CHECK(BaseInput::HapticDuration(0.25f) == 250000000LL);
	// None, or a bad value: the shortest pulse the runtime offers
	CHECK(BaseInput::HapticDuration(0.0f) == XR_MIN_HAPTIC_DURATION);
	CHECK(BaseInput::HapticDuration(-1.0f) == XR_MIN_HAPTIC_DURATION);
	CHECK(BaseInput::HapticDuration(std::numeric_limits<float>::quiet_NaN()) == XR_MIN_HAPTIC_DURATION);
	// Never a negative count, however long
	CHECK(BaseInput::HapticDuration(1e30f) == 3600000000000LL);
	CHECK(BaseInput::HapticDuration(std::numeric_limits<float>::infinity()) == XR_MIN_HAPTIC_DURATION);
}

TEST_CASE("Haptic_amplitude_clamped")
{
	CHECK(BaseInput::HapticAmplitude(0.5f, 1.0f) == doctest::Approx(0.5f));
	CHECK(BaseInput::HapticAmplitude(0.5f, 0.5f) == doctest::Approx(0.25f));
	CHECK(BaseInput::HapticAmplitude(7.0f, 1.0f) == 1.0f);
	CHECK(BaseInput::HapticAmplitude(-3.0f, 1.0f) == 0.0f);
	CHECK(BaseInput::HapticAmplitude(std::numeric_limits<float>::quiet_NaN(), 1.0f) == 0.0f);
}

TEST_CASE("Only_boolean_actions_read_as_digital")
{
	using T = BaseInput::ActionType;
	CHECK(BaseInput::DigitalReadError(T::Boolean) == vr::VRInputError_None);
	for (T type : { T::Vector1, T::Vector2, T::Vector3, T::Vibration, T::Pose, T::Skeleton, T::EyeTracking })
		CHECK(BaseInput::DigitalReadError(type) == vr::VRInputError_WrongType);
}

TEST_CASE("Active_set_hands")
{
	using H = BaseInput::SetHands;
	// Unrestricted: both hands, and the secondary set is ignored
	H all = BaseInput::HandsForActiveSet(-1);
	CHECK((all.primary[0] && all.primary[1]));
	CHECK((!all.secondary[0] && !all.secondary[1]));
	// A hand: that hand, with the secondary set on the other
	H left = BaseInput::HandsForActiveSet(0);
	CHECK((left.primary[0] && !left.primary[1]));
	CHECK((!left.secondary[0] && left.secondary[1]));
	H right = BaseInput::HandsForActiveSet(1);
	CHECK((!right.primary[0] && right.primary[1]));
	CHECK((right.secondary[0] && !right.secondary[1]));
	// A device that isn't a hand: no hand for the set, both for the secondary
	H head = BaseInput::HandsForActiveSet(2);
	CHECK((!head.primary[0] && !head.primary[1]));
	CHECK((head.secondary[0] && head.secondary[1]));
}

TEST_CASE("Input_source_paths")
{
	using S = BaseInput::InputSource;
	using A = BaseInput::HandAlias;
	auto left = BaseInput::ClassifyInputSource("/user/hand/left");
	CHECK(left.type == S::HAND_LEFT);
	CHECK(left.devicePath == "/user/hand/left");
	CHECK(left.alias == A::NONE);
	auto below = BaseInput::ClassifyInputSource("/user/hand/right/input/trigger/value");
	CHECK(below.type == S::HAND_RIGHT);
	CHECK(below.devicePath == "/user/hand/right");
	// Another word that starts with a hand's path isn't that hand
	CHECK(BaseInput::ClassifyInputSource("/user/hand/leftblah").type == S::INVALID);
	CHECK(BaseInput::ClassifyInputSource("/user/hand/rightfoo/input").type == S::INVALID);

	auto primary = BaseInput::ClassifyInputSource("/user/hand/primary");
	CHECK(primary.alias == A::PRIMARY);
	CHECK(BaseInput::ClassifyInputSource("/user/hand/secondary/input/a").alias == A::SECONDARY);

	CHECK(BaseInput::ClassifyInputSource("/user/head").type == S::HEAD);
	CHECK(BaseInput::ClassifyInputSource("/user/gamepad/input/a/click").type == S::OTHER);
	CHECK(BaseInput::ClassifyInputSource("/user/treadmill").type == S::OTHER);
	CHECK(BaseInput::ClassifyInputSource("/user/eyes").type == S::OTHER);
	auto tracker = BaseInput::ClassifyInputSource("/user/vive_tracker_htcx/role/waist/input/grip");
	CHECK(tracker.type == S::OTHER);
	CHECK(tracker.devicePath == "/user/vive_tracker_htcx");
	CHECK(BaseInput::ClassifyInputSource("/user/nothing").type == S::INVALID);
	CHECK(BaseInput::ClassifyInputSource("").type == S::INVALID);
}
