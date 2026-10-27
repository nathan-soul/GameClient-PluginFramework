// The mirrored countdown, compiled and exercised outside the engine.
//
//   ./tools/run_countdown_mirror_test.sh
//
// The cases that matter are the ones the field produced: a member arming from the host's start line,
// the same member cancelling when the lobby resets readiness (someone changed the map), and neither
// of those firing on the host itself (the host has its own countdown and must not mirror).
#include "GameClient/GencastCountdownMirror.h"

#include <cstdio>

static int g_failed = 0;
static int g_checked = 0;

static void expect(bool condition, const char* what)
{
	++g_checked;

	if (!condition)
	{
		++g_failed;
		std::printf("FAIL %s\n", what);
	}
}

int main()
{
	const long long now = 1793133715841LL;

	// Nothing armed = nothing to say.
	{
		GencastMirrorCountdown m;

		expect(!m.active(now), "a fresh mirror is inactive");
		expect(!m.cancelIfReadyChanged(12345), "a fresh mirror cannot be cancelled");
	}

	// Armed from the host's line: five seconds, from this client's own clock.
	{
		GencastMirrorCountdown m;

		const unsigned ready = GencastReadySignatureStep(GencastReadySignatureStep(0u, 0, true), 1, true);

		m.arm(now, 5, ready);

		expect(m.active(now), "armed mirror is active immediately");
		expect(m.active(now + 4999), "armed mirror is active with 1 ms left");
		expect(!m.active(now + 5000), "armed mirror is over at its end");
		expect(m.endsAtUnixMs == now + 5000, "the end is now plus five seconds");
		expect(!m.cancelIfReadyChanged(ready), "the same readiness does not cancel");
		expect(m.active(now + 1000), "and it keeps running");
	}

	// Cancelled by the lobby change: one player goes un-ready and the mirror stops.
	{
		GencastMirrorCountdown m;

		const unsigned before = GencastReadySignatureStep(0u, 0, true);
		const unsigned after = GencastReadySignatureStep(0u, 0, false);

		m.arm(now, 5, before);

		expect(m.cancelIfReadyChanged(after), "a changed readiness cancels");
		expect(!m.active(now + 1), "a cancelled mirror is inactive");
		expect(m.endsAtUnixMs == 0, "a cancelled mirror forgets its end");
		expect(!m.cancelIfReadyChanged(after), "cancelling twice is not a second cancel");
	}

	// An unreadable duration still counts down the lobby default rather than stopping at zero.
	{
		GencastMirrorCountdown m;

		m.arm(now, 0, 0u);

		expect(m.active(now + 4000), "a zero duration falls back to five seconds");
		expect(!m.active(now + 5000), "and still ends after five");
	}

	// A second start replaces the first: the host cancelled and started again, and the mirror follows.
	{
		GencastMirrorCountdown m;

		m.arm(now, 5, 7u);
		m.arm(now + 2000, 5, 7u);

		expect(m.endsAtUnixMs == now + 7000, "a re-arm moves the end out");
	}

	// The signature: a member going un-ready must change it, an unchanged lobby must not.
	{
		const unsigned a = GencastReadySignatureStep(GencastReadySignatureStep(0u, 0, true), 1, true);
		const unsigned b = GencastReadySignatureStep(GencastReadySignatureStep(0u, 0, false), 1, true);
		const unsigned c = GencastReadySignatureStep(GencastReadySignatureStep(0u, 0, true), 1, true);

		expect(a != b, "one un-ready player changes the signature");
		expect(a == c, "the same lobby has the same signature");
	}

	std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);

	return g_failed == 0 ? 0 : 1;
}
