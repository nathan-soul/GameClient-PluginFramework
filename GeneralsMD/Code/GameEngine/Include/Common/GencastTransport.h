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
	std::string wsUrl;      ///< the media socket the watcher connects to
	std::string sessionUrl; ///< the relay's pre-game session feed (/direct/session/{id})
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

/*
**	One watch attempt, as the server answered it. Transports report what happened; they do not
**	decide how long to keep trying. That policy is shared, because the two modes differ only in
**	which status codes mean what.
*/
enum class EGencastAttempt
{
	Success,
	NotFound,		// not watchable yet: the announce (relay) or liveness report (GO) has not landed
	Held,			// the server is holding this viewer; Services admission control only
	WrongPassword,	// must be shown as a wrong password, never as a signed-out session
	Failed
};

struct GencastAttemptResult
{
	EGencastAttempt kind = EGencastAttempt::Failed;
	std::string sessionId;
	std::string wsUrl;
	std::string sessionUrl;
	std::string streamUrl;
	std::string errorMessage;
	int holdSeconds = 0;
};

using GencastWatchListCallback = std::function<void(bool success, const std::vector<GencastWatchEntry>& entries)>;
using GencastSessionCallback = std::function<void(const GencastWatchSession& session)>;
using GencastRegisterCallback = std::function<void(const GencastStreamRegistration& registration)>;

/// Asked between attempts by the shared watch policy. TRUE means "stop trying": the caller is
/// going away, so the retry window must not be able to hold its thread.
using GencastCancelPredicate = std::function<bool()>;

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
	//
	// This is not implemented per transport. The bounded retry while a session is not yet
	// watchable, and the reporting of a hold, are the same problem in both modes and live in
	// RequestWatchSession() below. A transport only says what the server answered.
	//
	// cancelled is polled between attempts and during the wait: the observer's close() joins
	// the thread that calls this, so a viewer who leaves mid-retry must not wait out the whole
	// window. Unset means "never cancelled".
	virtual GencastAttemptResult AttemptWatchSession(const std::string& lobbyId,
		const std::string& password) = 0;

	void RequestWatchSession(const std::string& lobbyId, const std::string& password,
		GencastSessionCallback done,
		GencastCancelPredicate cancelled = GencastCancelPredicate());

	// Called by the shared policy when the viewer's hold state changes: waiting behind an
	// admission hold or a broadcast delay. Services sets it, the relay never does.
	virtual void OnHoldStateChanged(bool serverHeld, bool waitingForDelay) {}

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
