// The watcher's countdown decision, compiled and exercised outside the engine.
//
//   ./tools/run_countdown_plan_test.sh
//
// Every branch of GencastPlanCountdown has a case here, including the two that failed in the field:
// a utc_clock end time read as Unix time (27 s away, refused as too-far) and a second countdown
// started while the view was still in its "waiting for the stream" phase (refused, silently, now
// accepted).
#include "GameClient/GencastCountdownPlan.h"

#include <cstdio>
#include <string>

static int g_failed = 0;
static int g_checked = 0;

static void expectPlan(long long endsAt, long long now, bool gone, bool idle, bool waiting,
                       bool wantAccepted, int wantSeconds, const char* wantReason, const char* what)
{
	const GencastCountdownPlan plan = GencastPlanCountdown(endsAt, now, gone, idle, waiting);

	const bool ok = plan.accepted == wantAccepted && plan.seconds == wantSeconds &&
		std::string(plan.reason) == wantReason;

	if (!ok)
	{
		std::printf("FAIL %s: accepted=%d seconds=%d reason=%s (wanted accepted=%d seconds=%d reason=%s)\n",
			what, plan.accepted, plan.seconds, plan.reason,
			wantAccepted, wantSeconds, wantReason);
		++g_failed;
	}

	++g_checked;
}

int main()
{
	const long long now = 1793133715841LL;

	// The case that must work: the host announced an end 4.3 s away.
	expectPlan(now + 4300, now, false, true, false, true, 5, GENCAST_COUNTDOWN_OK,
		"4300 ms ahead starts at 5");
	expectPlan(now + 5000, now, false, true, false, true, 5, GENCAST_COUNTDOWN_OK,
		"5000 ms ahead starts at 5");
	expectPlan(now + 1, now, false, true, false, true, 1, GENCAST_COUNTDOWN_OK,
		"1 ms ahead starts at 1, never 0");
	expectPlan(now + 40, now, false, true, false, true, 1, GENCAST_COUNTDOWN_OK,
		"40 ms ahead rounds up to 1");
	expectPlan(now + 15000, now, false, true, false, true, 15, GENCAST_COUNTDOWN_OK,
		"the window includes its own edge");

	// The stale utc_clock value: 27 s of skew made a 5 s countdown look 32 s away.
	expectPlan(now + 32000, now, false, true, false, false, 0, GENCAST_COUNTDOWN_TOO_FAR,
		"a 32 s lead is refused, not clamped");

	// An end that has already passed, and one exactly on the tick.
	expectPlan(now, now, false, true, false, false, 0, GENCAST_COUNTDOWN_OVER,
		"an end at exactly now is over");
	expectPlan(now - 500, now, false, true, false, false, 0, GENCAST_COUNTDOWN_OVER,
		"an end in the past is over");

	// Phases. A second countdown from "waiting for the stream" is the case that used to vanish.
	expectPlan(now + 5000, now, false, false, true, true, 5, GENCAST_COUNTDOWN_OK,
		"a countdown is accepted while waiting for the stream");
	expectPlan(now + 5000, now, false, false, false, false, 0, GENCAST_COUNTDOWN_PHASE,
		"a countdown is refused while the view is joining");
	expectPlan(now + 5000, now, true, true, false, false, 0, GENCAST_COUNTDOWN_GONE,
		"a countdown is refused for a lobby that is gone");

	std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);

	return g_failed == 0 ? 0 : 1;
}
