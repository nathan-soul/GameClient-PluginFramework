#pragma once

#include <cstdint>
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

/*
**	The phase a lobby is being announced in.
**
**	Direct mode announces a lobby as soon as the player creates or joins it, long before there is
**	a match, so every announce has to say which of the two it is reporting: a watch list full of
**	"live" entries that are still seating players sends viewers at streams that do not exist, and
**	one full of "pregame" entries with a match running hides the matches.
*/
enum class EGencastStreamState
{
	Pregame,	// the players are still in the lobby
	Live		// the match is running
};

/// The relay's spelling of a phase ("pregame" or "live"); never NULL.
const char* GencastStreamStateName(EGencastStreamState state);

struct GencastWatchEntry
{
	std::string lobbyId;
	// Which match of that lobby this row is. A lobby can list a finished game next to the next
	// one, so the id is what names the game the viewer clicked; empty when the relay has none.
	std::string matchId;
	std::string mapName;
	// The host's title for the lobby. Kept apart from mapName so the watch list can show both;
	// empty when the relay has none (an older client's announce never carried it).
	std::string lobbyName;
	// The lobby's minimum watch delay in seconds, or -1 when the relay did not say. Display
	// only: the delay the relay applies arrives on the ROLE frame, not from here.
	int delaySeconds = -1;
	// The host's match-start countdown, as the relay mirrors it from the lobby's announce. In
	// direct mode this is the only source: GO's own observer push needs the /observers report,
	// which a plain relay install leaves unconfigured.
	bool countdownStarted = false;
	long long countdownEndsAtUnixMs = 0;
	std::string playersJson;
	// "pregame" while the players are seating, "live" while the match runs, "ended" during the
	// relay's post-game hold: still watchable, no longer a lobby.
	std::string state;
	bool hasPassword = false;
};

/// One lobby chat line as the relay hands it to a watcher. `kind` is "player" for the host's
/// players and "watcher" for another viewer - the same two lanes GO has.
struct GencastLobbyChat
{
	std::string from;
	std::string text;
	std::string kind;
	long long atUnixMs = 0;
};

/// One seat of the host's lobby, as the announcement reported it. `state` is the relay's spelling:
/// "open", "closed", "player", "easy_ai", "medium_ai", "brutal_ai".
struct GencastLobbySlot
{
	int slotIndex = 0;
	std::string state;
	std::string displayName;
	bool ready = false;
	bool isAi = false;
};

/// One answer of the pregame feed. Everything the lobby view needs to look like the host's own
/// lobby: the phase, the countdown, the title, the watch delay, and the chat since the last answer.
struct GencastPregameEvents
{
	bool live = false;
	bool countdownStarted = false;
	long long countdownEndsAtUnixMs = 0;
	std::string lobbyName;
	int delaySeconds = -1;
	int startsInSeconds = -1;
	std::vector<GencastLobbySlot> slots;
	std::vector<GencastLobbyChat> chat;
	long long next = 0;
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
	/// The polling feed of a lobby that has not started yet (/direct/pregame/{id}/events). Non-empty
	/// exactly when the join named a pregame session instead of a watch session.
	std::string pregameUrl;

	/// The polling lane of that feed (`/direct/pregame/{id}/events`), when the relay names it.
	///
	/// A separate field rather than a suffix because that is exactly the bug this replaces: the
	/// client appended its cursor to `pregame_url` — the socket route — and every fetch came back
	/// refused, with nothing logged. Empty from an older relay.
	std::string eventsUrl;
	/// False when the relay answered with a pregame session: nothing to watch yet, and the same
	/// session becomes the watch session when the stream attaches.
	bool live = true;
	std::string errorMessage;
	// 409: this account already watches this match, so no session was issued. The two fields
	// are the relay's own account of that other connection, and they exist so the player can be
	// told when and from where before deciding to disconnect it.
	bool duplicate = false;
	std::string duplicateSince;
	std::string duplicateClient;
};

/*
**	The relay's answer to "may this player stream this match?".
**
**	Advisory only: the client asks so it can keep a switch it could not honour out of the UI,
**	and the relay checks the same rule again when a stream is announced. It is deliberately
**	not a value this client ever sends back as proof - a can_stream field from the client is
**	exactly the claim the relay stopped trusting.
**
**	Fail closed: mayStream defaults to false, and a relay that could not be asked at all (no
**	answer, 404, unreadable body, missing field) leaves it false. Streaming needs an explicit
**	yes; a broken question is never an answer.
*/
struct GencastStreamPermission
{
	bool canStream = false;   ///< the player's own right, as the relay stores it
	bool matchEnabled = false;///< this match already accepts extra sources
	bool mayStream = false;   ///< canStream || matchEnabled: the relay's verdict
	// "player_enabled", "match_enabled" or "denied" while the relay answered. Empty when it did
	// not, which is the same refusal as "denied" but says why nobody can explain it.
	std::string reason;
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
	// 409: this account already has a watch session for this match. Not retryable and not a
	// password problem - only the player can say whether that other connection may be dropped.
	Duplicate,
	Failed
};

struct GencastAttemptResult
{
	EGencastAttempt kind = EGencastAttempt::Failed;
	// The server's own status, 0 when no answer arrived. The kind above is the policy-relevant
	// part; the status is what lets a caller that only asks "would this password be accepted?"
	// tell a dropped lobby (410) from a relay with direct watch switched off (403).
	int httpStatus = 0;
	std::string sessionId;
	std::string wsUrl;
	std::string sessionUrl;
	std::string streamUrl;
	/// Non-empty when the relay answered with a pregame session: the lobby exists but has not
	/// started, and this is the feed to read it from. A caller that wants to watch must wait.
	std::string pregameUrl;

	/// The polling lane of that feed (`/direct/pregame/{id}/events`), as the relay names it.
	///
	/// Separate from `pregameUrl` because that is the bug this replaces: the client appended its
	/// cursor to the socket route and every fetch was refused, with nothing logged.
	std::string eventsUrl;
	/// False on a pregame answer; true for every answer that names a watch session, and for older
	/// relays that send neither field.
	bool live = true;
	std::string errorMessage;
	int holdSeconds = 0;
	// Duplicate only: when the other session started and which client it came from, as the relay
	// reports them. Empty when the relay did not say.
	std::string duplicateSince;
	std::string duplicateClient;
};

using GencastWatchListCallback = std::function<void(bool success, const std::vector<GencastWatchEntry>& entries)>;
using GencastSessionCallback = std::function<void(const GencastWatchSession& session)>;
using GencastRegisterCallback = std::function<void(const GencastStreamRegistration& registration)>;
using GencastStreamPermissionCallback = std::function<void(const GencastStreamPermission& permission)>;

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
	// matchId names which match of that lobby to watch, and is empty when the caller does not
	// know one: the relay then picks the newest game that has not ended, which is what every
	// caller did before match ids existed. A caller that does know one must say it, or a viewer
	// who clicked a finished match gets the next one instead.
	//
	// cancelled is polled between attempts and during the wait: the observer's close() joins
	// the thread that calls this, so a viewer who leaves mid-retry must not wait out the whole
	// window. Unset means "never cancelled".
	//
	// watcherId is the GO user id of the viewer. Every join carries it: a watch session belongs
	// to an account, so it is what the relay gates the group and the per-watcher password on,
	// and what lets it refuse a second connection for the same match. There is no anonymous
	// path - a transport that cannot name the viewer fails the join instead of asking.
	//
	// kickExisting asks the relay to drop a session this account already has on this match
	// (409 -> retry). Only ever set after the player confirmed that takeover.
	/// One chat line from a watcher into the host's lobby, in direct mode. The relay stamps nothing:
	/// `from` is the watcher's own name and `kind` is "watcher", so the host's players can tell the
	/// two lanes apart.
	virtual bool PostLobbyChat(const std::string& lobbyId, const std::string& from,
		const std::string& kind, const std::string& text)
	{
		return false;
	}

	/// The pregame feed, for a client with no push lane: one request brings the lobby as the host's
	/// client announced it, every chat line after `since`, and the cursor to ask from next time.
	/// Returns false when the feed is not available (services mode, no session, or a refusal).
	virtual bool FetchPregameEvents(const std::string& eventsUrl, long long since,
		GencastPregameEvents& out)
	{
		return false;
	}

	virtual GencastAttemptResult AttemptWatchSession(const std::string& lobbyId,
		const std::string& password, const std::string& matchId, uint64_t watcherId,
		bool kickExisting) = 0;

	void RequestWatchSession(const std::string& lobbyId, const std::string& password,
		const std::string& matchId, uint64_t watcherId, bool kickExisting,
		GencastSessionCallback done,
		GencastCancelPredicate cancelled = GencastCancelPredicate());

	// May this player stream this match? Asked when the player enters a lobby and again when a
	// stream announce is refused, because the answer is per player and per match rather than per
	// install: the admin can grant the right, and another source can open the match's pool.
	//
	// The result is advisory UI state, never an authority. The relay decides again when a
	// stream is announced; this only keeps a switch out of a UI that could not honour it.
	//
	// done runs on the calling thread - the request is synchronous like every other call here -
	// and is always called exactly once, with mayStream false when the relay could not be asked.
	virtual void QueryStreamPermission(uint64_t userId, const std::string& matchId,
		GencastStreamPermissionCallback done) = 0;

	// Direct mode: tell the relay that this account exists, once, when the client starts.
	//
	// A viewer who never hosts has no row at the relay, and the join gate refuses an account it
	// cannot find (403) - so without this a pure watcher cannot watch anything at all. Nothing
	// else in this client speaks for a player who only watches: the announce and the permission
	// question both need a lobby, and a viewer may never enter one.
	//
	// userId is GetUserID() and displayName the name GO knows. There is no client id parameter
	// on purpose: the relay stores it on the player row so a takeover dialog can name the
	// connection, which means it has to be the id the join sends - one value, from one place.
	//
	// Fire-and-forget. The request goes out on a worker, a failure is a log line and never a
	// dialog, and the only thing reported back is the watcher's group and whether that group
	// needs a password. Never a password: the relay does not return one either.
	virtual void RegisterWatcher(uint64_t userId, const std::string& displayName) {}

	// Called by the shared policy when the viewer's hold state changes: waiting behind an
	// admission hold or a broadcast delay. Services sets it, the relay never does.
	virtual void OnHoldStateChanged(bool serverHeld, bool waitingForDelay) {}

	// The phase is the caller's to state: the same lobby is announced while the players are still
	// seating and again once its match runs, and the relay shows whichever it was told last. It
	// is a parameter rather than transport state so that a stale value can never be announced: the
	// caller that knows the match started is the one that says so.
	virtual void RegisterStream(const std::string& lobbyId, EGencastStreamState state,
		GencastRegisterCallback done) = 0;

	// Direct mode: keep the relay's entry for a lobby in step with the lobby itself, without
	// waiting for a match and without reporting the answer. RegisterStream only runs once a
	// stream is starting, which leaves a lobby invisible for the whole pregame; this is what
	// makes it watchable before anyone streams.
	//
	// The lobby is read on the calling thread and the request is sent from a worker: the
	// transport's calls are synchronous, and this one is reached from a lobby refresh, where a
	// five-second timeout inside a frame would freeze the lobby. An announce that would not
	// change anything the relay stores is dropped, so a lobby that is refreshed far more often
	// than it changes costs one request per change, not one per refresh.
	//
	// Failures are logged, never reported: nobody is waiting on the answer.
	virtual void AnnounceLobby(const std::string& lobbyId, EGencastStreamState state) {}

	// Direct mode: one line of lobby chat, so a watcher of a pregame lobby reads the same
	// conversation its players do.
	//
	// Only the host's client sends this. It is the one client with the lobby's live
	// transcript, and it is already the announcer the relay binds the entry to - a member
	// forwarding the same line would post it twice. `from` is the speaker's display name and
	// `text` the message without the "[Name] " prefix GO added, because the relay names the
	// line itself.
	//
	// Fire-and-forget, sent from a worker for the same reason the announce is: the request
	// is synchronous with a five second timeout and this is reached from the websocket tick
	// on the game thread. A relay that is absent or older is a log line, never a dialog.
	virtual void ForwardLobbyChat(const std::string& from, const std::string& text) {}

	// Direct mode has to tell the relay who is announcing and what the lobby looks like: the
	// relay binds that player's password to the lobby on the first announce, keyed by the
	// Services user id. Services mode ignores all of it, because Services already knows who
	// you are.
	//
	// playerId is the wire slot id (1-based) and stays the fallback identity; userId is the GO
	// Services id the relay prefers, and <= 0 means "not signed in". displayName and mapName
	// are display data only, and so are lobbyName (the host's title) and delaySeconds (the
	// lobby's minimum watch delay, -1 when unknown) - the watch list shows both, and neither
	// decides anything about the stream.
	virtual void SetAnnounceContext(long playerId, long userId, const std::string& displayName,
		const std::string& mapName, const std::string& lobbyName = std::string(),
		int delaySeconds = -1) {}

	/*
	**	The relay's lobby lifecycle, from the client's side.
	**
	**	The client reports the same two events to the relay that it reports to GO Services - "I
	**	left that lobby" and "the match is over" - and the relay decides what each means. It does
	**	not know whether it is the host disbanding a lobby or a member walking out of somebody
	**	else's, and it must not guess: the relay already has the announcer binding, so it can.
	**
	**	Both are fire-and-forget: nobody waits on the answer, both are reached from the game
	**	thread (leaving a lobby, the end of a match) and the request is sent from a worker. Match
	**	ids are GO's uint64 identity for the game, not the lobby: one lobby can list a finished
	**	match next to the next one.
	*/
	virtual void UnannounceLobby(const std::string& lobbyId, uint64_t matchId) {}
	virtual void EndMatch(const std::string& lobbyId, uint64_t matchId, const char* reason) {}

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

/*
**	Announce the lobby the local player is in, if direct mode has anything new to say about it.
**
**	Called by the lobby whenever the lobby it holds changes: on create, on join, and on every
**	update GO pushes afterwards. No-op outside direct mode, with live streaming switched off, or
**	when the local player has no seat to announce under yet.
**
**	Lives here rather than in the lobby code so that everything direct mode decides - who is
**	announcing, in which phase, and whether anything actually changed - is in one place.
*/
void GencastAnnounceCurrentLobby();

/// Whether a lobby's stream is known to be live: the lobby observer view's one question, asked
/// the way the current mode can answer it. Live is final for the caller; false means "not yet",
/// either because the probe is in flight, because it was asked too recently, or because the mode
/// answers by push instead (services mode: GO sends STREAM_LIVE once the relay reports /observers).
struct GencastLobbyStreamState
{
	bool live = false;                  ///< the relay says a stream is running in this lobby
	bool countdownStarted = false;      ///< the host's match-start countdown is running
	long long countdownEndsAtUnixMs = 0;///< wall-clock end of that countdown, 0 when unknown

	/// True when the relay refused the feed because this lobby's group wants its watch password
	/// and none was supplied: the viewer has to be asked before the preview opens.
	bool passwordRequired = false;

	/// True when the answer came from the relay's pregame feed rather than the lobby list: the
	/// rest of the fields are then the lobby as the host's own client sees it (slots, title, delay,
	/// chat), which is what the lobby view renders in direct mode.
	bool fromFeed = false;

	std::string lobbyName;              ///< the host's lobby title, empty when never said
	int delaySeconds = -1;              ///< the lobby's minimum watch delay, -1 when never said
	int startsInSeconds = -1;           ///< seconds until playback, -1 when not live

	/// The host's seats, when the answer came from the feed. Empty for a lobby-list answer.
	std::vector<GencastLobbySlot> slots;

	/// Chat lines that arrived since the previous answer, oldest first. Drained by the view.
	std::vector<GencastLobbyChat> chat;
};

/// One non-blocking step of that question. Returns true when a fresh answer was available and
/// fills <paramref name="out"/>; false while the probe is still in flight or was just made.
/// One step of "what is this lobby doing". `password` is the watch password the viewer has for
/// this lobby's group, empty when none has been asked for yet; the feed needs it exactly like the
/// watch does, so a passworded lobby's preview stays closed until it is typed.
bool GencastPollLobbyStreamState(const AsciiString& lobbyId, const AsciiString& password,
	GencastLobbyStreamState& out);

/// The pregame feed's push lane: the relay's WebSocket for the session the join handed out.
///
/// Start it once the feed URL is known (the lobby view does that when it opens) and stop it when
/// the view closes or the stream hands off. While it is connected its frames are the truth; the
/// poll above stays as the fallback for a relay that cannot upgrade.
void GencastPregameSocketStart();
void GencastPregameSocketStop();
bool GencastPregameSocketConnected();

/// Hands over the newest pushed state, with the chat lines that arrived since the last call, and
/// returns false when nothing new arrived (the caller keeps what it already drew).
bool GencastPregameSocketTake(GencastLobbyStreamState& out);

/// Sends a watcher's own lobby line to the relay, on a worker. Direct mode only; the line comes
/// back through the feed with the next answer, exactly like everyone else's.
void GencastPostWatcherChat(const AsciiString& lobbyId, const AsciiString& text);

/*
**	Mirror one lobby chat line to the relay, when direct mode has an entry to attach it to.
**
**	Called from the LOBBY_CHAT_FROM_SERVER case, which every client in the lobby runs once
**	per line. Only the host forwards: the relay keeps one announcer per entry, and the other
**	members would post the same line again. isAnnouncement marks GO's system lines ("Observers
**	watching: ...", the countdown ticks), which are not player chat and are left out.
**
**	The speaker is resolved here rather than at the call site: GO puts the name in user_id and
**	formats the message as "[Name] text", and the relay names the line from `from`, so the body
**	it receives must not carry the name twice.
**
**	No-op outside direct mode, when this client is not the lobby's host, and when nothing is
**	left to say after the name comes off. Failures are logged, never reported: nobody waits on
**	a chat forward.
*/
void GencastForwardLobbyChat(int64_t fromUserId, const std::string& message, bool isAnnouncement);

void GencastNoteLobbyAnnouncement(const std::string& message, bool isAnnouncement);
void GencastNoteHostCountdownStarted();

/// A lobby line this client typed itself. Whoever types posts: the old path only forwarded the
/// host's copy of GO's echo, so a lobby whose host was not the streamer published no chat at all.
void GencastPostOwnLobbyChat(const std::string& message);

/*
**	"Nothing here belongs to us any more" and "our match is over", reported to the relay.
**
**	Same funnel as the announce and for the same reason: the caller says what happened in the
**	game, and everything direct mode needs to turn that into a request - which lobby, which
**	match, under which player id - is resolved here. No-op outside direct mode, so callers need
**	no mode test of their own.
**
**	UnannounceCurrentLobby goes on the single leave funnel
**	(NGMP_OnlineServices_LobbyInterface::LeaveCurrentLobby), beside the DELETE that tells GO
**	Services, because host-disband and member-leave are the same event to this client: "this
**	player is no longer in that lobby". The relay decides which of the two it was.
**
**	EndCurrentMatch is for the announcer of a live match that just finished; it starts the
**	relay's post-game hold instead of leaving the entry looking live until it is reaped.
*/
void GencastUnannounceCurrentLobby();
void GencastEndCurrentMatch(const char* reason);

/*
**	"Register me as a viewer" - once per process start, in direct mode.
**
**	Called from the end of a successful GO login (NGMP_OnlineServicesManager::OnLogin). That is
**	the earliest moment both halves of the question are known: the GO user id the relay keys the
**	player row on, and the mode this client is in. Deliberately not hung off the transport's own
**	mode detection or the lobby announce: those are reached from a lobby refresh, and a pure
**	watcher never enters a lobby - which is exactly the account this call exists for.
**
**	Once per process start even when login completes again (a token refresh that has to log in
**	reaches the same hook). No-op outside direct mode, so callers need no mode test of their own,
**	and no-op without a user id: the registration is never worth inventing an account for. A
**	relay that cannot be reached must not cost the player a login, a menu or a game, so the
**	request is sent from a worker with a couple of retries and every outcome is one log line.
*/
void GencastRegisterWatcher();

/*
**	The relay's streaming permission for the lobby the local player is in.
**
**	A player may stream when the admin granted the right, or when the match they are in already
**	accepts sources - so the answer belongs to (player, match) and has to be asked for, cached,
**	and asked again when the situation changes: on entering a lobby, and after an announce was
**	refused.
**
**	Fail closed. Until the relay has actually said yes for this lobby and this match, the answer
**	here is no: a 404, a timeout, an unreadable body or a body without may_stream all mean the
**	client does not know, and "does not know" must never be shown as "may". No stream is
**	announced on an unknown either - the whole point of asking first is not to make the relay
**	say no to something the player was shown as possible.
*/

/// Ask the relay for this player's permission on the lobby they are in. Runs on a worker (the
/// request is synchronous with a five second timeout, and this is reached from a lobby refresh),
/// and does nothing when the same question was answered moments ago. No-op outside direct mode.
void GencastRefreshStreamPermission();

/// Re-ask when the cached answer has gone stale, so a right the admin grants while the player
/// sits in a lobby shows up without leaving and re-entering it. Must be pumped from the lobby's
/// update: it starts at most one request per staleness window.
void GencastTickStreamPermission();

/// Ask the relay right now, on the calling thread, and cache the answer.
///
/// For the one caller that must not act on a cached guess: the streamer, about to announce. It
/// already blocks on the relay for that announce, so one more synchronous call costs nothing it
/// has not already paid - and it is the last place where "not granted" can still stop a request.
/// Returns FALSE unless the relay granted it, so a failed question refuses the announce.
bool GencastQueryStreamPermissionNow(void);

/// The relay's answer for the lobby the player is in, as last received. FALSE whenever it is not
/// an explicit yes - including "not asked yet" and "the relay could not be asked".
bool GencastMayStreamCurrentLobby(void);

/// Why: "player_enabled", "match_enabled" or "denied" when the relay answered, "" when it did
/// not. For wording, never for a decision.
std::string GencastStreamPermissionReason(void);
