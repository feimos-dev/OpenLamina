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
