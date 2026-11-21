#include "doctest.h"

#include "stdafx.h"

#include "XrBackend.h"

#include <chrono>

using Outcome = XrBackend::StateOutcome;

TEST_CASE("A lost session is made again, and only the user quitting quits")
{
	// LOSS_PENDING is a loss, whatever came before
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_LOSS_PENDING, false, false) == Outcome::Lost);
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_LOSS_PENDING, true, false) == Outcome::Lost);
	// EXITING after STOPPING is the user quitting from the runtime; without STOPPING, the session went away
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_EXITING, true, false) == Outcome::Quit);
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_EXITING, false, false) == Outcome::Lost);
	// OpenLamina ending the session itself (a restart) is neither
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_EXITING, true, true) == Outcome::Ignored);
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_LOSS_PENDING, false, true) == Outcome::Ignored);
	// Other states mean nothing for the game here
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_STOPPING, false, false) == Outcome::Ignored);
	CHECK(XrBackend::OutcomeOfState(XR_SESSION_STATE_IDLE, true, false) == Outcome::Ignored);
}

TEST_CASE("The event pump looks for the controllers only on a running session nothing else has input on")
{
	// From the session's first frames on, with no actions, no legacy input and no hand found
	CHECK(XrBackend::ControllerQueryDue(XR_SESSION_STATE_SYNCHRONIZED, false, false, false, false, false));
	CHECK(XrBackend::ControllerQueryDue(XR_SESSION_STATE_VISIBLE, false, false, false, false, false));
	CHECK(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, false, false, false, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_UNKNOWN, false, false, false, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_IDLE, false, false, false, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_READY, false, false, false, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_STOPPING, false, false, false, false, false));
	// Not while the session is being ended
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, true, false, false, false, false));
	// Not on a session being made for the game's actions, even one that is focused as soon as it begins
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, false, true, false, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_SYNCHRONIZED, false, true, false, false, false));
	// Not once the game has its own input, or a hand has its controller
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, false, false, true, false, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, false, false, false, true, false));
	CHECK_FALSE(XrBackend::ControllerQueryDue(XR_SESSION_STATE_FOCUSED, false, false, false, false, true));
}

TEST_CASE("A refused frame is ended again with fewer layers only when a layer may be why")
{
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_LAYER_INVALID));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_LAYER_LIMIT_EXCEEDED));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_SWAPCHAIN_RECT_INVALID));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_POSE_INVALID));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_VALIDATION_FAILURE));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_RUNTIME_FAILURE));
	CHECK(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_HANDLE_INVALID));
	// Nothing fewer layers would change
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_SESSION_LOST));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_INSTANCE_LOST));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_SESSION_NOT_RUNNING));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_CALL_ORDER_INVALID));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_TIME_INVALID));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED));
	// Not a refusal at all
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_SUCCESS));
	CHECK_FALSE(XrBackend::EndFrameRefusalMayBeALayer(XR_SESSION_LOSS_PENDING));
}

TEST_CASE("The system is asked for at once after a loss, then about once a second")
{
	using clock = std::chrono::steady_clock;
	const clock::time_point t = clock::now();
	CHECK(XrBackend::RecoveryDue({}, t));
	CHECK_FALSE(XrBackend::RecoveryDue(t, t));
	CHECK_FALSE(XrBackend::RecoveryDue(t, t + std::chrono::milliseconds(999)));
	CHECK(XrBackend::RecoveryDue(t, t + XrBackend::kRecoveryInterval));
	CHECK(XrBackend::RecoveryDue(t, t + std::chrono::seconds(30)));
}
