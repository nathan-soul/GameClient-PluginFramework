#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"
#include "Common/GlobalData.h"
#include "Common/GencastClientLog.h"   // GencastDebugLog, for the permission lines
#include "Common/LiveObserver.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
// The pregame announce reads the player's Services id, and OnlineServices_Init.h only forward
// declares the auth interface.
#include "GameNetwork/GeneralsOnline/OnlineServices_Auth.h"
// The pregame announce reads the lobby the player is sitting in; the transport itself needs
// LobbyEntry/LobbyMemberEntry for that.
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
// the announce also asks the game whether a match is running (GameInfo::isGameInProgress)
#include "GameNetwork/GeneralsOnline/NGMPGame.h"

#include <curl/curl.h>

#include <cstdio>
#include <memory>
#include <mutex>

namespace
{
	/*
	**	A HEAD against the Services livestream endpoint.
	**
	**	404 means the deployment does not carry the livestream routes at all — vanilla Services.
	**	That is "this client cannot gencast", which must hide the Watch Live menu. It is not a
	**	sign-in problem, and it must not be reported as one.
	*/
	bool ServicesHasLivestreams()
	{
		const AsciiString endpoint = liveServicesEndpoint("Livestreams");

		if (endpoint.isEmpty())
		{
			return false;
		}

		CURL* curl = curl_easy_init();

		if (curl == nullptr)
		{
			return false;
		}

		curl_easy_setopt(curl, CURLOPT_URL, endpoint.str());
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

		const CURLcode result = curl_easy_perform(curl);

		long httpStatus = 0;

		if (result == CURLE_OK)
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
		}

		curl_easy_cleanup(curl);

		return result == CURLE_OK && httpStatus != 404;
	}

	/*
	**	Is this client in direct mode?
	**
	**	The same test GencastTransport::DetermineMode() makes before it probes Services, without
	**	the probe itself. Shared so the mode is decided in one place: the resolve helpers below
	**	and the watcher registration must not be able to disagree about whether there is a relay
	**	to talk to at all.
	*/
	bool DirectModeConfigured()
	{
		return NGMP_OnlineServicesManager::Settings.Relay_Direct() &&
			!NGMP_OnlineServicesManager::Settings.Relay_Direct_Url().empty();
	}
}

const char* GencastModeName(EGencastMode mode)
{
	switch (mode)
	{
		case EGencastMode::Direct:
			return "direct";
		case EGencastMode::Services:
			return "services";
		case EGencastMode::Unavailable:
		default:
			return "unavailable";
	}
}

const char* GencastStreamStateName(EGencastStreamState state)
{
	switch (state)
	{
		case EGencastStreamState::Pregame:
			return "pregame";
		case EGencastStreamState::Live:
			return "live";
	}

	// Unreachable, and pregame is the safe answer if it ever is: an entry reported live with no
	// match behind it sends viewers at a stream that does not exist.
	return "pregame";
}

EGencastMode GencastTransport::DetermineMode()
{
	const bool directConfigured = NGMP_OnlineServicesManager::Settings.Relay_Direct();
	const std::string relayUrl = NGMP_OnlineServicesManager::Settings.Relay_Direct_Url();

	if (directConfigured && !relayUrl.empty())
	{
		return EGencastMode::Direct;
	}

	if (!ServicesHasLivestreams())
	{
		return EGencastMode::Unavailable;
	}

	return EGencastMode::Services;
}

/*
**	The shared watch policy.
**
**	A watcher can arrive in the instant between clicking and the stream becoming watchable, so
**	"not found" is retried for a bounded window rather than reported as a dead end. A hold is
**	reported to the observer, which owns the state the game reads to draw the waiting view. A
**	wrong password stops immediately: retrying it would lock the player out of their own lobby.
*/
void GencastTransport::RequestWatchSession(const std::string& lobbyId,
	const std::string& password, const std::string& matchId, uint64_t watcherId,
	bool kickExisting, GencastSessionCallback done, GencastCancelPredicate cancelled)
{
	// 20s covers the announce/liveness race; anything longer feels like a hang.
	const DWORD startedAt = timeGetTime();
	const DWORD retryWindowMs = 20000;
	const DWORD retryCadenceMs = 500;

	GencastWatchSession session;

	// The cancel exit reports through done() like every other exit, so the callback contract
	// ("invoked exactly once") holds no matter how the wait ends.
	const std::function<void()> reportCancelled = [&]()
	{
		session.errorMessage = "Cancelled.";
		OnHoldStateChanged(false, false);
		done(session);
	};

	for (;;)
	{
		// A viewer who left while this was running must not be held here: LiveObserver::close()
		// joins the thread that calls this, so without the check the whole window would be a
		// freeze on the way out of the game.
		if (cancelled && cancelled())
		{
			reportCancelled();
			return;
		}

		const GencastAttemptResult attempt =
			AttemptWatchSession(lobbyId, password, matchId, watcherId, kickExisting);

		switch (attempt.kind)
		{
			case EGencastAttempt::Success:
				session.success = true;
				session.sessionId = attempt.sessionId;
				session.wsUrl = attempt.wsUrl;
				session.sessionUrl = attempt.sessionUrl;
				session.streamUrl = attempt.streamUrl;
				session.httpStatus = 200;
				OnHoldStateChanged(false, false);
				done(session);
				return;

			case EGencastAttempt::WrongPassword:
				// 401 carries real meaning here and must not be retried or reframed.
				session.httpStatus = 401;
				session.errorMessage = attempt.errorMessage.empty()
					? "Wrong password." : attempt.errorMessage;
				OnHoldStateChanged(false, false);
				done(session);
				return;

			case EGencastAttempt::Duplicate:
				// Another session for this account is watching this match. Retrying the same
				// question cannot change that, and deciding to drop the other connection is the
				// player's call, so it is reported intact and the caller asks.
				session.httpStatus = 409;
				session.duplicate = true;
				session.duplicateSince = attempt.duplicateSince;
				session.duplicateClient = attempt.duplicateClient;
				session.errorMessage = attempt.errorMessage.empty()
					? "This account is already watching this game." : attempt.errorMessage;
				OnHoldStateChanged(false, false);
				done(session);
				return;

			case EGencastAttempt::Held:
				OnHoldStateChanged(true, attempt.holdSeconds > 0);
				break;

			case EGencastAttempt::NotFound:
				break;

			case EGencastAttempt::Failed:
				session.errorMessage = attempt.errorMessage.empty()
					? "The server did not answer." : attempt.errorMessage;
				OnHoldStateChanged(false, false);
				done(session);
				return;
		}

		if (timeGetTime() - startedAt >= retryWindowMs)
		{
			session.errorMessage = attempt.kind == EGencastAttempt::Held
				? "Still held by the server."
				: "That lobby is not watchable yet.";
			OnHoldStateChanged(false, false);
			done(session);
			return;
		}

		// Wait in slices so a cancel landing mid-cadence is seen within ~50ms instead of
		// holding the joining thread for the rest of the cadence.
		for (DWORD waited = 0; waited < retryCadenceMs; waited += 50)
		{
			if (cancelled && cancelled())
			{
				reportCancelled();
				return;
			}

			Sleep(50);
		}
	}
}

GencastTransport* GencastTransport::Create()
{
	const EGencastMode mode = DetermineMode();

	DEBUG_LOG(("[GENCAST] mode=%s", GencastModeName(mode)));

	if (mode == EGencastMode::Direct)
	{
		return CreateRelayGencastTransport();
	}

	// Services mode keeps using the calls it already makes; there is no second implementation
	// of today's behaviour to hand back yet.
	return nullptr;
}

/*
**	The identity here comes from the lobby, not from the player list.
**
**	ThePlayerList is not built until the match starts, so the local Player that the streamer
**	announces under does not exist while the players are seating. The lobby member carries the
**	same numbers, and the seat index has to be the one the streamer will use as its wire slot:
**	the relay refuses the stream to a lobby whose bound announcer announced under a different
**	player id, and GO seats players contiguously from slot 0, so the two agree.
*/
namespace
{
	/*
	**	Who is speaking, about which game, in direct mode.
	**
	**	One resolver for the three lifecycle calls (announce, unannounce, end), because they have
	**	to agree: an unannounce that names a different player than the announce was sent under is
	**	a no-op at the relay, and an end that names the wrong match starts a post-game hold on
	**	somebody else's game. Reading it once also keeps the "is direct mode even on" and "is this
	**	player seated" tests in a single place.
	*/
	struct DirectLobbyIdentity
	{
		std::string lobbyId;	// GO's LobbyID as decimal text, spelled the way GO prints it
		uint64_t matchId = 0;	// GetCurrentMatchID(); 0 is a client bug, reported by the user
		long playerId = 0;		// 1-based wire slot, the id the relay binds the announce to
		long userId = 0;		// GO Services user id; the relay prefers it as player identity
		std::string displayName;
		std::string mapName;	// GO's map name, empty until the host has picked one
	};

	bool ResolveDirectLobbyIdentity(DirectLobbyIdentity& out)
	{
		if (!DirectModeConfigured())
		{
			// Without a URL the transport resolves to the Services mode, whose reachability probe
			// (a blocking HEAD) has no business running from a lobby refresh.
			return false;
		}

		// Switched off means this client would not announce at match start either. An entry
		// nobody can stream is worse than no entry: the watch list would advertise a lobby a
		// viewer cannot be given a session for.
		if (TheGlobalData == nullptr || !TheGlobalData->m_liveStreamEnabled)
		{
			return false;
		}

		NGMP_OnlineServices_LobbyInterface* lobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (lobbyInterface == nullptr || !lobbyInterface->IsInLobby())
		{
			return false;
		}

		NGMP_OnlineServices_AuthInterface* authInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

		if (authInterface == nullptr)
		{
			return false;
		}

		const int64_t userId = authInterface->GetUserID();

		if (userId <= 0)
		{
			// No Services id to bind the announce to. Anything invented here would attach a
			// stranger's password to this lobby.
			return false;
		}

		const LobbyEntry& lobby = lobbyInterface->GetCurrentLobby();

		const LobbyMemberEntry* localMember = nullptr;

		for (const LobbyMemberEntry& member : lobby.members)
		{
			if (member.user_id == userId)
			{
				localMember = &member;
				break;
			}
		}

		if (localMember == nullptr || localMember->m_SlotIndex == LobbyMemberEntry::SLOT_INDEX_NONE)
		{
			// In the lobby but not seated yet: there is no wire slot to announce under, and being
			// seated is itself a lobby change, so this runs again as soon as the seat is known.
			return false;
		}

		// GO's identity for the game in this lobby. Set before the announce runs (UpdateRoomDataCache
		// stores it, then calls the announce), and never 0 for a real lobby; the relay keys its
		// entry on it, so the three calls have to quote the same one.
		out.matchId = lobbyInterface->GetCurrentMatchID();

		char lobbyId[32];
		snprintf(lobbyId, sizeof(lobbyId), "%lld", (long long)lobby.lobbyID);
		out.lobbyId = lobbyId;

		// 1-based wire slot, as the streamer announces it: the relay rejects player id 0 outright
		// and Generals' slot indices are 0-based.
		out.playerId = (long)localMember->m_SlotIndex + 1;
		out.userId = (long)userId;
		out.displayName = localMember->display_name;
		out.mapName = lobby.map_name;

		return true;
	}

	/*
	**	The (account, game) a streaming-permission question is about.
	**
	**	Deliberately not ResolveDirectLobbyIdentity above: that one insists on a seat, because an
	**	announce has to name the wire slot it speaks under. A permission question does not - it is
	**	about the account and the match - and refusing to ask it because no seat is known would
	**	report "no right" for a player who has one, at the one moment (match start) where the
	**	answer decides whether a stream happens at all. It also ignores this client's local
	**	streaming preference, which says nothing about what the relay will grant.
	*/
	bool ResolveStreamPermissionIdentity(DirectLobbyIdentity& out)
	{
		if (!DirectModeConfigured())
		{
			return false;
		}

		NGMP_OnlineServices_LobbyInterface* lobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (lobbyInterface == nullptr || !lobbyInterface->IsInLobby())
		{
			return false;
		}

		NGMP_OnlineServices_AuthInterface* authInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

		if (authInterface == nullptr)
		{
			return false;
		}

		const int64_t userId = authInterface->GetUserID();

		if (userId <= 0)
		{
			// The right hangs on an account, and there is none to ask about.
			return false;
		}

		const LobbyEntry& lobby = lobbyInterface->GetCurrentLobby();

		char lobbyId[32];
		snprintf(lobbyId, sizeof(lobbyId), "%lld", (long long)lobby.lobbyID);

		out.lobbyId = lobbyId;
		out.matchId = lobbyInterface->GetCurrentMatchID();
		out.userId = (long)userId;
		out.playerId = 0;			// not needed for a permission question
		out.displayName.clear();
		out.mapName = lobby.map_name;

		return true;
	}

	/*
	**	The relay's streaming permission, cached per lobby and match.
	**
	**	One cached answer rather than one per caller: the lobby's stream switch and the streamer's
	**	own announce must agree, and two independent questions could be answered differently a
	**	second apart. Keyed on (lobby, match) because a right to stream one game says nothing
	**	about the next game in the same lobby, and using the stale answer either way would either
	**	hide a switch the relay would honour or offer one it would refuse.
	**
	**	Fail closed: the cached permission starts false and only ever becomes true from an explicit
	**	relay answer for this exact game.
	*/
	std::mutex s_permissionMutex;
	std::string s_permissionLobbyId;
	std::string s_permissionMatchId;
	GencastStreamPermission s_permission;
	bool s_permissionAnswered = false;
	bool s_permissionInFlight = false;
	DWORD s_permissionAnsweredAtMs = 0;

	// A lobby announce that was skipped because the permission was not known to be granted.
	// GencastTickStreamPermission() re-runs it once the relay says yes, otherwise a lobby whose
	// first announce landed before the answer would stay unpublished until something else about
	// it changed.
	bool s_announceHeldForPermission = false;

	// Answers older than this are asked again: the admin can enable a player mid-lobby, and the
	// rule is per match, so a pool that opens while the player is waiting must show up too.
	const DWORD kPermissionFreshMs = 15000;

	struct StreamPermissionKey
	{
		std::string lobbyId;
		std::string matchId;
		uint64_t userId = 0;
	};

	void PublishStreamPermission(const StreamPermissionKey& key,
		const GencastStreamPermission& permission)
	{
		std::scoped_lock<std::mutex> lock(s_permissionMutex);

		s_permissionInFlight = false;

		if (key.lobbyId != s_permissionLobbyId || key.matchId != s_permissionMatchId)
		{
			// The player is looking at another game now, or left. This answer describes a game
			// nobody is asking about, and publishing it would answer the next question wrongly.
			GencastDebugLog("gencast: stream permission lobby=%s match=%s stale, dropped",
				key.lobbyId.c_str(), key.matchId.c_str());
			return;
		}

		s_permission = permission;
		s_permissionAnswered = true;
		s_permissionAnsweredAtMs = timeGetTime();
	}

	void StreamPermissionThread(StreamPermissionKey key)
	{
		// Default-constructed: mayStream false, reason empty. Anything that goes wrong below -
		// no transport, no answer, an unreadable body - leaves it that way, which is the whole
		// fail-closed rule.
		GencastStreamPermission permission;

		// Created directly rather than through GencastTransport::Create(): the mode was already
		// decided on the main thread, and Create() re-runs the reachability probe, which can
		// reach GO with a blocking HEAD from a thread that has no business touching it.
		std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

		if (transport)
		{
			transport->QueryStreamPermission(key.userId, key.matchId,
				[&permission](const GencastStreamPermission& answer) { permission = answer; });
		}

		GencastDebugLog("gencast: stream permission lobby=%s match=%s may=%d can=%d enabled=%d reason=%s",
			key.lobbyId.c_str(), key.matchId.c_str(), permission.mayStream ? 1 : 0,
			permission.canStream ? 1 : 0, permission.matchEnabled ? 1 : 0,
			permission.reason.empty() ? "(no answer)" : permission.reason.c_str());

		PublishStreamPermission(key, permission);
	}
}

/*
**	Ask the relay whether this player may stream the game they are in.
**
**	Deliberately quiet about failure: nobody is waiting on the answer, and a relay that cannot be
**	asked must not turn into a dialog every time a lobby is refreshed. The answer is cached for
**	the switch and the announce to read; not knowing is reported as "no" by
**	GencastMayStreamCurrentLobby().
*/
void GencastRefreshStreamPermission()
{
	DirectLobbyIdentity identity;

	// Not ResolveDirectLobbyIdentity: the permission belongs to the account, not to this client's
	// local "will I broadcast" preference, and it can be asked before a seat exists.
	if (!ResolveStreamPermissionIdentity(identity))
	{
		return;
	}

	const uint64_t userId = identity.userId > 0 ? (uint64_t)identity.userId : 0;
	const std::string matchId = std::to_string(identity.matchId);

	std::scoped_lock<std::mutex> lock(s_permissionMutex);

	if (identity.lobbyId != s_permissionLobbyId || matchId != s_permissionMatchId)
	{
		// A different game: its answer does not apply, so forget the old one entirely rather
		// than let it stand until the new one arrives.
		s_permissionLobbyId = identity.lobbyId;
		s_permissionMatchId = matchId;
		s_permission = GencastStreamPermission();
		s_permissionAnswered = false;
		s_permissionAnsweredAtMs = 0;
	}

	if (userId == 0)
	{
		// No Services id means no account to ask about, and /direct/stream-permission would be
		// asked about nobody. The answer stays "no".
		return;
	}

	if (s_permissionInFlight)
	{
		return;
	}

	if (s_permissionAnswered && (DWORD)(timeGetTime() - s_permissionAnsweredAtMs) < kPermissionFreshMs)
	{
		return;
	}

	StreamPermissionKey key;
	key.lobbyId = identity.lobbyId;
	key.matchId = matchId;
	key.userId = userId;

	s_permissionInFlight = true;

	try
	{
		std::thread(StreamPermissionThread, key).detach();
	}
	catch (...)
	{
		// No worker, so no answer will arrive. Clearing the flag is what lets the next refresh
		// try again instead of believing a question is still in the air forever.
		s_permissionInFlight = false;
	}
}

void GencastTickStreamPermission()
{
	GencastRefreshStreamPermission();

	if (!s_announceHeldForPermission)
	{
		return;
	}

	if (!GencastMayStreamCurrentLobby())
	{
		return;
	}

	// The permission arrived after the lobby's own announce was skipped. Nothing else about the
	// lobby may change for a while - players sit ready in a full lobby - so this is the only
	// place that can publish it.
	s_announceHeldForPermission = false;
	GencastAnnounceCurrentLobby();
}

bool GencastQueryStreamPermissionNow(void)
{
	DirectLobbyIdentity identity;

	if (!ResolveStreamPermissionIdentity(identity))
	{
		// No lobby, no account: there is no right to grant, and the answer is no.
		return false;
	}

	const uint64_t userId = (uint64_t)identity.userId;
	const std::string matchId = std::to_string(identity.matchId);

	// Default-constructed, so every failure below leaves "may not stream" standing.
	GencastStreamPermission permission;

	std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

	if (transport)
	{
		transport->QueryStreamPermission(userId, matchId,
			[&permission](const GencastStreamPermission& answer) { permission = answer; });
	}

	{
		std::scoped_lock<std::mutex> lock(s_permissionMutex);

		// This is the newest word on this game, so it becomes the cached one too: the lobby's
		// switch and the streamer must not disagree about the same match.
		s_permissionLobbyId = identity.lobbyId;
		s_permissionMatchId = matchId;
		s_permission = permission;
		s_permissionAnswered = true;
		s_permissionAnsweredAtMs = timeGetTime();
	}

	GencastDebugLog("gencast: stream permission (blocking) lobby=%s match=%s -> may=%d reason=%s",
		identity.lobbyId.c_str(), matchId.c_str(), permission.mayStream ? 1 : 0,
		permission.reason.empty() ? "(no answer)" : permission.reason.c_str());

	return permission.mayStream;
}

bool GencastMayStreamCurrentLobby(void)
{
	DirectLobbyIdentity identity;

	if (!ResolveStreamPermissionIdentity(identity))
	{
		// Not in a lobby, not signed in, or not direct mode: there is nothing to stream here.
		return false;
	}

	std::scoped_lock<std::mutex> lock(s_permissionMutex);

	if (identity.lobbyId != s_permissionLobbyId ||
		std::to_string(identity.matchId) != s_permissionMatchId)
	{
		// No answer about this game yet. Fail closed: not knowing is not permission.
		return false;
	}

	return s_permission.mayStream;
}

std::string GencastStreamPermissionReason(void)
{
	DirectLobbyIdentity identity;

	if (!ResolveStreamPermissionIdentity(identity))
	{
		return std::string();
	}

	std::scoped_lock<std::mutex> lock(s_permissionMutex);

	if (identity.lobbyId != s_permissionLobbyId ||
		std::to_string(identity.matchId) != s_permissionMatchId)
	{
		return std::string();
	}

	return s_permission.reason;
}

void GencastAnnounceCurrentLobby()
{
	DirectLobbyIdentity identity;

	if (!ResolveDirectLobbyIdentity(identity))
	{
		return;
	}

	// The match is announced live by the streamer; everything the lobby reports after that point
	// is still a live lobby, never a pregame one - a refresh landing mid-match must not downgrade
	// the relay's entry.
	const EGencastStreamState state =
		(TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress())
			? EGencastStreamState::Live
			: EGencastStreamState::Pregame;

	// GO's map name is the one the lobby shows, and it is UTF-8 already. It is empty until the
	// host has picked a map, and TheGlobalData->m_mapName is the map that was last *loaded* -
	// still the shell map when a match starts, which is how "ShellMapMD" reached the relay and
	// would have reached the watch list. Keep the last map actually published instead, and stay
	// quiet while there has never been one: an entry with no map is worse than no entry yet.
	static std::mutex s_mapMutex;
	static std::string s_lastMapName;

	std::string mapName = identity.mapName;

	{
		std::scoped_lock<std::mutex> lock(s_mapMutex);

		if (!mapName.empty())
		{
			s_lastMapName = mapName;
		}
		else
		{
			mapName = s_lastMapName;
		}
	}

	if (mapName.empty())
	{
		return;
	}

	// Streaming is the relay's right to give, so ask for it and announce nothing until it has
	// actually said yes for this game. Fail closed: a relay that predates the endpoint (404), one
	// that does not answer, and one whose body has no may_stream all mean "not granted", and a
	// lobby published anyway would be a promise this client cannot keep - the watch list would
	// advertise a game whose stream the relay is going to refuse.
	GencastRefreshStreamPermission();

	if (!GencastMayStreamCurrentLobby())
	{
		// Held, not dropped: the announcement is retried by GencastTickStreamPermission() the
		// moment the relay's answer arrives. A lobby that is merely waiting for its answer must
		// not stay invisible until something unrelated about it changes.
		s_announceHeldForPermission = true;

		GencastDebugLog("gencast: announce lobby=%s held, no stream permission yet (%s)",
			identity.lobbyId.c_str(), GencastStreamPermissionReason().c_str());

		return;
	}

	s_announceHeldForPermission = false;

	std::unique_ptr<GencastTransport> transport(GencastTransport::Create());

	if (transport == nullptr)
	{
		return;
	}

	transport->SetAnnounceContext(identity.playerId, identity.userId, identity.displayName, mapName);
	transport->AnnounceLobby(identity.lobbyId, state);
}

/*
**	The leave funnel and the match end, resolved the same way the announce is.
**
**	Both hand the request to the transport, which sends it from a worker: these are called from
**	LeaveCurrentLobby and from the Recorder's teardown, on the game thread, and the relay's
**	five second timeout has no place in a frame.
**
**	Created through CreateRelayGencastTransport() rather than GencastTransport::Create(): the
**	mode is already known to be direct (the resolver just tested the settings), and Create()
**	re-runs the probe, which can reach GO with a blocking HEAD.
*/
void GencastUnannounceCurrentLobby()
{
	DirectLobbyIdentity identity;

	if (!ResolveDirectLobbyIdentity(identity))
	{
		return;
	}

	std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

	if (transport == nullptr)
	{
		return;
	}

	transport->SetAnnounceContext(identity.playerId, identity.userId, identity.displayName,
		identity.mapName);
	transport->UnannounceLobby(identity.lobbyId, identity.matchId);
}

void GencastEndCurrentMatch(const char* reason)
{
	DirectLobbyIdentity identity;

	if (!ResolveDirectLobbyIdentity(identity))
	{
		return;
	}

	std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

	if (transport == nullptr)
	{
		return;
	}

	transport->SetAnnounceContext(identity.playerId, identity.userId, identity.displayName,
		identity.mapName);
	transport->EndMatch(identity.lobbyId, identity.matchId, reason);
}

/*
**	"I am here, and I only want to watch."
**
**	The relay's join gate refuses an account it has no player row for, and a pure viewer has no
**	other way to get one: it never announces a lobby and never asks for stream permission, which
**	are the only two calls that create a row. Registering here is what turns that 403 into a
**	session for somebody who has never hosted anything.
**
**	Once per process start. OnLogin can complete a second time - a token refresh that has to log
**	in again reaches it - and the registration is idempotent at the relay, but "once per client
**	start" is the contract and a repeated request at a rate-limited endpoint is not worth the
**	thing it would not change.
*/
void GencastRegisterWatcher()
{
	static std::once_flag s_registered;

	// Checked before the flag, never inside it: a call that arrives before the settings are
	// loaded (or in a stock install, where direct mode never turns on) must not consume the one
	// attempt this process has.
	if (!DirectModeConfigured())
	{
		return;
	}

	NGMP_OnlineServices_AuthInterface* authInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

	if (authInterface == nullptr || authInterface->GetUserID() <= 0)
	{
		// Login reported success without an account. There is nothing to register, and spending
		// the flag here would keep the next - real - login from doing it.
		GencastDebugLog("gencast: watcher register skipped, no GO user id");
		return;
	}

	const uint64_t userId = (uint64_t)authInterface->GetUserID();
	std::string displayName = authInterface->GetDisplayName();

	// The auth interface's own "nothing has arrived yet" value, not a name. Sending it would
	// replace a display name the relay already knows with a placeholder.
	if (displayName == "NO_USER")
	{
		displayName.clear();
	}

	try
	{
		std::call_once(s_registered, [userId, displayName]()
			{
				// Built here rather than through GencastTransport::Create(): the mode is already
				// known (the check above), and Create() re-runs the Services reachability probe,
				// which is a blocking HEAD nobody asked for.
				std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

				if (transport)
				{
					transport->RegisterWatcher(userId, displayName);
				}
			});
	}
	catch (...)
	{
		// No worker, or no transport: the registration is a convenience for the next join, so it
		// may never become an exception on the way out of a successful login.
		GencastDebugLog("gencast: watcher register user=%llu skipped, no worker",
			(unsigned long long)userId);
	}
}

#endif // GENERALS_ONLINE
