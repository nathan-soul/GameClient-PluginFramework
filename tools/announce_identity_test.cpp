// The announce identity, compiled and exercised outside the engine.
//
//   ./tools/run_announce_identity_test.sh
//
// The case that matters is the one the relay showed: lobby 416797 announced as "416797" through
// the whole pregame, and then as "match:4633018" five seconds later, which is two games in the
// list and a pregame entry that never goes live.
#include "GameClient/GencastAnnounceIdentity.h"

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
	// Pregame: GO has no match id yet, so the lobby id is the identity.
	{
		GencastAnnounceIdentity identity;
		bool changed = false;

		const unsigned long long pinned = identity.pin("416797", 0, changed);

		expect(pinned == 0, "a pregame lobby pins match id 0");
		expect(changed, "the first pin is reported as a change");
		expect(!identity.pin("416797", 0, changed) && !changed, "the same lobby does not re-pin");
	}

	// The bug: the match starts, GO reports an id, and the pinned identity must not move.
	{
		GencastAnnounceIdentity identity;
		bool changed = false;

		identity.pin("416797", 0, changed);

		const unsigned long long duringMatch = identity.pin("416797", 4633018, changed);

		expect(duringMatch == 0, "a match id that appears later does not move the entry");
		expect(!changed, "and it is not reported as a change");
	}

	// Before the first announce: a client that already knows the id keeps it for the whole game.
	{
		GencastAnnounceIdentity identity;
		bool changed = false;

		expect(identity.pin("416800", 4633020, changed) == 4633020, "a known match id is pinned as-is");
		expect(identity.pin("416800", 0, changed) == 4633020, "and a later 0 does not unpin it");
	}

	// The next game is a new lobby, so it gets its own pin.
	{
		GencastAnnounceIdentity identity;
		bool changed = false;

		identity.pin("416797", 0, changed);

		expect(identity.pin("416830", 0, changed) == 0, "a new lobby pins its own identity");
		expect(changed, "a new lobby is a change");
		expect(identity.pin("416797", 0, changed) == 0 && changed, "going back to the old lobby re-pins it");
	}

	// clear() forgets the pin, for a client that leaves the lobby entirely.
	{
		GencastAnnounceIdentity identity;
		bool changed = false;

		identity.pin("416797", 0, changed);
		identity.clear();

		expect(identity.pin("416797", 4633018, changed) == 4633018, "after clear the next value is pinned");
	}

	std::printf("%s: %d checks, %d failed\n", g_failed == 0 ? "PASS" : "FAIL", g_checked, g_failed);

	return g_failed == 0 ? 0 : 1;
}
