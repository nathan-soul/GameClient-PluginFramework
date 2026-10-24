#pragma once

#include <functional>
#include <string>
#include <vector>

/*
**	Gencast transport.
**
**	The client can gencast two ways: through our own Services (what it does today) or straight
**	at the relay, with no Services involvement at all. The rest of the game must not care which
**	one is in use, so every mode-specific detail lives behind this interface and callers only
**	ever say "give me the watch list" or "get me a session".
**
**	Relay_Direct defaults to false: a stock install takes the Services path and makes no new
**	network calls, exactly as before.
*/

enum class EGencastMode
{
	Unavailable,	// neither a custom Services deployment nor a relay is reachable
	Services,		// our Services mints the tickets, as today
	Direct			// the relay is asked directly, password-gated
};

const char* GencastModeName(EGencastMode mode);

struct GencastWatchEntry
{
	std::string lobbyId;
	std::string mapName;
	std::string playersJson;
	std::string state;
	bool hasPassword = false;
};

struct GencastWatchSession
{
	bool success = false;
	// 401 here means "wrong password" and must not be shown as "signed out".
	int httpStatus = 0;
	std::string sessionId;
	std::string wsUrl;
	std::string streamUrl;
	std::string errorMessage;
};

struct GencastStreamRegistration
{
	bool success = false;
	std::string streamKey;
	std::string streamUrl;
	std::string errorMessage;
};

using GencastWatchListCallback = std::function<void(bool success, const std::vector<GencastWatchEntry>& entries)>;
using GencastSessionCallback = std::function<void(const GencastWatchSession& session)>;
using GencastRegisterCallback = std::function<void(const GencastStreamRegistration& registration)>;

class GencastTransport
{
public:
	virtual ~GencastTransport() {}

	virtual EGencastMode Mode() const = 0;

	// May this client gencast at all? When false the Watch Live menu is hidden rather than
	// showing a sign-in prompt: a vanilla Services deployment has no livestream routes.
	virtual bool IsAvailable() const = 0;

	virtual void FetchWatchList(GencastWatchListCallback done) = 0;

	// Direct mode takes the password from the player; Services mode ignores it and mints from
	// the session token, which is why the password is a parameter rather than transport state.
	virtual void RequestWatchSession(const std::string& lobbyId, const std::string& password,
		GencastSessionCallback done) = 0;

	virtual void RegisterStream(const std::string& lobbyId, GencastRegisterCallback done) = 0;

	// Direct mode has to tell the relay who is announcing and what the lobby looks like: the
	// relay binds that player's password to the lobby on the first announce. Services mode
	// ignores all of it, because Services already knows who you are.
	virtual void SetAnnounceContext(long playerId, const std::string& mapName,
		const std::string& playersCsv) {}

	virtual void SubscribeLobbyUpdates(const std::string& lobbyId) = 0;
	virtual void UnsubscribeLobbyUpdates() = 0;

	// Resolves the mode once, from settings plus a reachability probe.
	static EGencastMode DetermineMode();

	// Builds the implementation for the resolved mode. Callers create this once per session and
	// keep it; it is never re-created per call.
	static GencastTransport* Create();
};

// Defined in GencastTransportRelay.cpp; only meaningful in Direct mode.
GencastTransport* CreateRelayGencastTransport();
