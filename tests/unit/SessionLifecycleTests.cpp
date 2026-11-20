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
