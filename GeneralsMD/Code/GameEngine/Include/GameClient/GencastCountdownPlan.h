#pragma once

/*
**  GencastCountdownPlan - the one decision the watcher's lobby screen makes about the host's
**  countdown, pulled out of the menu so it can be compiled and exercised without the engine.
**
**  Why this exists: the countdown ran once already and could not be told apart from one that never
**  arrived, because the decision was buried in a 1500-line GUI callback and both failure paths were
**  silent. Here the rules are a pure function with a name, and tools/countdown_plan_test.cpp pins
**  every branch - the 0..15000 ms window, the ceiling to whole seconds, the phases that may start a
**  countdown and the ones that may not.
**
**  No engine types, no logging, no clock: the caller passes the wall clock in, so the test can too.
*/

/// The longest a countdown may still be away and still be this screen's to run. The host's countdown
/// is 5 s; the slack covers the announce travelling back to a watcher, and anything beyond it is not
/// a countdown that is starting but a stale or mis-clocked value (a utc_clock stamp read as Unix
/// time is 27 s off and lands here, which is how the first version failed).
#define GENCAST_COUNTDOWN_MAX_LEAD_MS 15000

struct GencastCountdownPlan
{
	bool accepted;      ///< run the countdown on this screen
	int seconds;        ///< whole seconds to start from, 0 when refused
	const char* reason; ///< "ok", or why not - one string per refusal, so a log line can name it
};

/// Reasons, as string literals: the log line carries the reason and no allocation is needed.
#define GENCAST_COUNTDOWN_OK        "ok"
#define GENCAST_COUNTDOWN_GONE      "lobby-gone"
#define GENCAST_COUNTDOWN_PHASE     "phase"
#define GENCAST_COUNTDOWN_OVER      "already-over"
#define GENCAST_COUNTDOWN_TOO_FAR   "too-far"

/// Decides whether this screen runs the host's countdown, and from which second.
///
/// - refused when the lobby is gone;
/// - refused from any phase except idle ("nothing running yet") or waiting ("a countdown already
///   ran out and the stream has not arrived"): a host who starts a second match from the same view
///   is a real case, and dropping it in silence is what made the feature look broken;
/// - refused when the end has already passed, or is further away than the window above;
/// - otherwise the countdown starts at ceil(remaining / 1000), never below 1, so 4300 ms shows 5
///   and 40 ms shows 1.
inline GencastCountdownPlan GencastPlanCountdown(long long endsAtUnixMs, long long nowUnixMs,
                                                 bool lobbyGone, bool phaseIdle, bool phaseWaiting)
{
	if (lobbyGone)
	{
		return { false, 0, GENCAST_COUNTDOWN_GONE };
	}

	if (!phaseIdle && !phaseWaiting)
	{
		return { false, 0, GENCAST_COUNTDOWN_PHASE };
	}

	const long long remainingMs = endsAtUnixMs - nowUnixMs;

	if (remainingMs <= 0)
	{
		return { false, 0, GENCAST_COUNTDOWN_OVER };
	}

	if (remainingMs > GENCAST_COUNTDOWN_MAX_LEAD_MS)
	{
		return { false, 0, GENCAST_COUNTDOWN_TOO_FAR };
	}

	long long seconds = (remainingMs + 999) / 1000;

	if (seconds < 1)
	{
		seconds = 1;
	}

	return { true, (int)seconds, GENCAST_COUNTDOWN_OK };
}
