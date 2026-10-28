#pragma once

#include <string>

/*
**  GencastAnnounceIdentity - the (lobby, match) pair a client repeats to the relay.
**
**  The relay keys a watch entry on the match id when it has one and on the lobby id otherwise
**  (WatchRegistry.KeyFor). GO mints the match id when the match is created, i.e. when the lobby's
**  countdown ends, so during pregame GetCurrentMatchID() is 0 and from the first frame of the
**  match it is not. A client that sends what it currently has therefore announces the same game
**  under two identities: the pregame entry keyed on the lobby id, and a second entry keyed on the
**  match id. Watchers who joined the pregame entry keep waiting on a lobby that never goes live,
**  while a duplicate game appears in the list.
**
**  So the identity is pinned by the first announce for a lobby, and every later announce, end and
**  unannounce for that lobby quotes the same pair. Lobbies are per game in GO, and a new lobby is a
**  new pin.
**
**  Engine-free so tools/announce_identity_test.sh can compile this exact header.
*/

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
