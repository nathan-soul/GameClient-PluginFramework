#pragma once

#include <string>

// The (lobby, match) pair a client repeats to the relay: GO mints the match id only when the
// match starts, so the first announce pins it. See tools/run_announce_identity_test.sh.

struct GencastAnnounceIdentity
{
	bool pinned = false;
	std::string lobbyId;
	unsigned long long matchId = 0;

	/// The match id every announce for this lobby must quote.
	///
	/// The first lobby seen pins whatever GO reports at that moment - 0 during pregame, which is
	/// the point: the entry must not move when the match id arrives. `changed` reports a fresh
	/// pin so the caller can log it once instead of on every announce.
	unsigned long long pin(const std::string& lobby, unsigned long long currentMatchId, bool& changed)
	{
		changed = false;

		if (!pinned || lobby != lobbyId)
		{
			pinned = true;
			lobbyId = lobby;
			matchId = currentMatchId;
			changed = true;
		}

		return matchId;
	}

	void clear()
	{
		pinned = false;
		lobbyId.clear();
		matchId = 0;
	}
};
