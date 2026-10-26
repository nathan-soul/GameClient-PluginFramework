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
#include "GameNetwork/GeneralsOnline/json.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

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

				// A pregame answer is not a session the observer can watch: the relay lists the
				// lobby but the streamer has not attached, and a watch socket would be refused.
				// Treated as "not ready yet" and retried inside this call's window, exactly like a
				// 404 - the same wait, whichever way the relay spells it.
				if (attempt.wsUrl.empty())
				{
					GencastDebugLog("gencast: join lobby=%s -> pregame session (%s), waiting for the stream",
						lobbyId.c_str(), attempt.pregameUrl.empty() ? "no url" : attempt.pregameUrl.c_str());
					break;
				}

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
		std::string lobbyName;	// the host's title for the lobby, display only
		int delaySeconds = -1;	// the lobby's minimum watch delay, -1 when GO has none
	};

	namespace
{
	// The match this client announced, kept past the GO lobby it came from.
	//
	// Leaving a match tears the lobby down first, so resolving the identity again at that moment
	// fails - IsInLobby() is already false - and the relay was never told the stream was over. That
	// is why a watcher sat on the last frame until the relay's reconnect grace ran out. The match a
	// client announced is exactly the match whose stream is ending, so it is remembered here.
	struct AnnouncedMatch
	{
		std::mutex mutex;
		bool valid = false;
		DirectLobbyIdentity identity;
	};

	AnnouncedMatch s_announcedMatch;

	void RememberAnnouncedMatch(const DirectLobbyIdentity& identity)
	{
		std::lock_guard<std::mutex> lock(s_announcedMatch.mutex);
		s_announcedMatch.valid = true;
		s_announcedMatch.identity = identity;
	}

	bool CachedAnnouncedMatch(DirectLobbyIdentity& out)
	{
		std::lock_guard<std::mutex> lock(s_announcedMatch.mutex);

		if (!s_announcedMatch.valid)
		{
			return false;
		}

		out = s_announcedMatch.identity;
		return true;
	}

	void ForgetAnnouncedMatch()
	{
		std::lock_guard<std::mutex> lock(s_announcedMatch.mutex);
		s_announcedMatch.valid = false;
	}
}

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
		// Display data for the watch list. The title is the host's own name for the lobby, and
		// the delay is the lobby's minimum watch delay; both may be absent (a lobby that has
		// not been named yet, or a GO that predates the field), which reads as empty/-1.
		out.lobbyName = lobby.name;
		out.delaySeconds = lobby.stream_delay_seconds;

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

	transport->SetAnnounceContext(identity.playerId, identity.userId, identity.displayName, mapName,
		identity.lobbyName, identity.delaySeconds);
	transport->AnnounceLobby(identity.lobbyId, state);

	// The relay now lists this match. Remember which one, so ending the stream does not depend on
	// the GO lobby still existing at that moment.
	RememberAnnouncedMatch(identity);
}

/*
**	Whether the lobby's stream is live, answered by whichever party can know it in this mode.
**
**	Direct mode has no push to wait for: the relay is the only party that sees a streamer
**	connect, and GO is only told about it through the relay's /observers report, which is
**	unconfigured on a plain relay install. So the relay's own lobby list is asked, on a detached
**	fetch - the transport is synchronous and must not be spent inside a frame. The answer is kept
**	for the requesting lobby only and handed out once, so a stale probe cannot start a watch for
**	the lobby the player has since left.
**
**	The answer carries the host's countdown too: a direct-mode viewer counts down from the same
**	absolute end the lobby's own members do, where services mode is pushed GAME_STARTING by GO.
*/
/// A watcher's own line, sent straight to the relay. Detached on purpose: the transport is
/// synchronous, and the lobby view's frame must not wait for a round-trip - the line appears when
/// the next feed answer carries it back, which is the same second.
void GencastPostWatcherChat(const AsciiString& lobbyId, const AsciiString& text)
{
	if (lobbyId.isEmpty() || text.isEmpty())
	{
		return;
	}

	const std::string id(lobbyId.str());
	const std::string body(text.str());

	std::thread([id, body]()
	{
		try
		{
			std::string from;

			NGMP_OnlineServices_AuthInterface* auth =
				NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

			if (auth != nullptr)
			{
				from = auth->GetDisplayName();
			}

			std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

			if (!transport)
			{
				return;
			}

			if (!transport->PostLobbyChat(id, from, "watcher", body))
			{
				GencastDebugLog("gencast: watcher chat was not delivered lobby=%s", id.c_str());
			}
		}
		catch (...)
		{
			// A chat line is not worth taking the view down for.
		}
	}).detach();
}

bool GencastPollLobbyStreamState(const AsciiString& lobbyId, const AsciiString& password,
	GencastLobbyStreamState& out)
{
	out = GencastLobbyStreamState();

	if (!NGMP_OnlineServicesManager::Settings.Relay_Direct())
	{
		// Services mode answers by push: GO sends GAME_STARTING and STREAM_LIVE to the lobby
		// view once the relay has reported the stream through /observers, so nothing is polled.
		return false;
	}

	const std::string id = lobbyId.str();

	static std::mutex s_mutex;
	static std::string s_answerFor;
	static GencastLobbyStreamState s_answer;
	static bool s_answered = false;
	static std::atomic<bool> s_running{ false };
	static std::chrono::steady_clock::time_point s_lastProbe{};

	{
		std::scoped_lock<std::mutex> lock(s_mutex);

		if (s_answered && s_answerFor == id)
		{
			out = s_answer;
			s_answerFor.clear();
			s_answered = false;
			return true;
		}
	}

	if (s_running.load())
	{
		return false;
	}

	const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

	// Four probes a second. Two seconds was the first number and it is what "the lobby feels
	// dead" was: a 5 s countdown polled at 2 s intervals lands late or not at all, and chat
	// arrives in lumps. The running flag still keeps one request in flight at a time.
	if (s_lastProbe.time_since_epoch().count() != 0 &&
		now - s_lastProbe < std::chrono::milliseconds(250))
	{
		return false;
	}

	s_lastProbe = now;
	s_running.store(true);

	// The feed belongs to one session, so its url and cursor are kept per lobby. They are reset
	// whenever the lobby changes: a cursor from another game would silently swallow this one's chat.
	static std::string s_feedLobby;
	static std::string s_feedUrl;
	static long long s_feedSince = 0;
	static bool s_feedAsked = false;
	static bool s_feedShapeLogged = false;
	static bool s_feedFailureLogged = false;
	static std::string s_feedPassword;

	const std::string watchedPassword(password.str());

	std::thread([id, watchedPassword]()
	{
		GencastLobbyStreamState answer;

		try
		{
			std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

			if (transport)
			{
				transport->FetchWatchList([&](bool success, const std::vector<GencastWatchEntry>& entries)
				{
					if (!success)
					{
						return;
					}

					for (const GencastWatchEntry& entry : entries)
					{
						if (entry.lobbyId != id)
						{
							continue;
						}

						// "live" only means a stream is running: an ended match is watchable but
						// is not a stream starting now, and the lobby hand-off is about a match
						// that just began.
						answer.live = entry.state == "live";
						answer.countdownStarted = entry.countdownStarted;
						answer.countdownEndsAtUnixMs = entry.countdownEndsAtUnixMs;
						break;
					}
				});

				// The lobby as the host's own client sees it - slots, title, delay, chat - comes
				// from the relay's pregame feed. It needs a session, which the join hands out for a
				// lobby that has not started; a lobby whose group wants a password answers 401 here
				// (the password prompt comes later, at watch time) and the list above stands alone.
				// A new password (or a new lobby) is a new question: the previous refusal was about
				// the credential, and holding the "already asked" flag would keep the preview shut
				// after the viewer typed it.
				if (s_feedLobby != id || s_feedPassword != watchedPassword)
				{
					s_feedUrl.clear();
					s_feedSince = 0;
					s_feedAsked = false;
					s_feedShapeLogged = false;
					s_feedFailureLogged = false;
					s_feedPassword = watchedPassword;
					s_feedLobby = id;
					answer.passwordRequired = false;
				}

				// Once per lobby, not once per probe: the answer cannot change without a password
				// the client does not have yet, and asking every second put a 401 in the relay's
				// log for every frame of the lobby view.
				if (s_feedUrl.empty() && !s_feedAsked)
				{
					s_feedAsked = true;
					uint64_t watcherId = 0;
					NGMP_OnlineServices_AuthInterface* auth =
						NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

					if (auth != nullptr && auth->GetUserID() > 0)
					{
						watcherId = (uint64_t)auth->GetUserID();
					}

					if (watcherId != 0)
					{
						// The plain join, not RequestWatchSession: this one wants the pregame answer
						// and must not sit in that call's 20 s retry window on a lobby that has
						// nothing to watch yet.
						const GencastAttemptResult joined =
							transport->AttemptWatchSession(id, watchedPassword, "", watcherId, false);

						s_feedLobby = id;

						if (joined.kind == EGencastAttempt::Success && !joined.pregameUrl.empty())
						{
							// The polling lane the relay named. Falling back to the socket URL only
						// keeps an older relay's answer from being fetched as a socket path - a
						// fetch that was refused silently, which made the whole lobby view empty.
						s_feedUrl = !joined.eventsUrl.empty() ? joined.eventsUrl : joined.pregameUrl;
							s_feedSince = 0;

							GencastDebugLog("gencast: pregame feed for lobby=%s at %s",
								id.c_str(), s_feedUrl.c_str());
						}
						else
						{
							// 401 is the one refusal the viewer can fix: the lobby's group wants a
							// password and this client has none. The view asks for it; every other
							// answer is final until the password changes.
							answer.passwordRequired = joined.httpStatus == 401;

							GencastDebugLog("gencast: no pregame feed for lobby=%s kind=%d status=%d",
								id.c_str(), (int)joined.kind, joined.httpStatus);
						}
					}
				}

				GencastPregameEvents events;

				const bool fetched = !s_feedUrl.empty() && s_feedLobby == id &&
					transport->FetchPregameEvents(s_feedUrl, s_feedSince, events);

				if (!fetched && !s_feedUrl.empty() && !s_feedFailureLogged)
				{
					// Silent failures are what made the last three reports unreadable: the fetch
					// was pointed at a socket route and the view just showed nothing.
					s_feedFailureLogged = true;
					GencastDebugLog("gencast: pregame feed refused lobby=%s url=%s", id.c_str(), s_feedUrl.c_str());
				}

				if (fetched)
				{
					answer.fromFeed = true;
					answer.lobbyName = events.lobbyName;
					answer.delaySeconds = events.delaySeconds;
					answer.startsInSeconds = events.startsInSeconds;
					answer.slots = events.slots;
					answer.chat = events.chat;

					if (events.live)
					{
						answer.live = true;
					}

					if (events.countdownStarted)
					{
						answer.countdownStarted = true;
						answer.countdownEndsAtUnixMs = events.countdownEndsAtUnixMs;
					}

					if (!s_feedShapeLogged)
					{
						s_feedShapeLogged = true;
						GencastDebugLog("gencast: pregame feed lobby=%s phase=%s slots=%d chat=%d next=%lld title=%s",
							id.c_str(), events.live ? "live" : "pregame", (int)events.slots.size(),
							(int)events.chat.size(), events.next, events.lobbyName.c_str());
					}

					s_feedSince = events.next;
				}
			}
		}
		catch (...)
		{
			answer = GencastLobbyStreamState();
		}

		{
			std::scoped_lock<std::mutex> lock(s_mutex);
			s_answerFor = id;
			s_answer = answer;
			s_answered = true;
		}

		s_running.store(false);
	}).detach();

	return false;
}

/*
**	One lobby chat line, mirrored so a watcher of the pregame lobby reads the same
**	conversation its players do.
**
**	The relay has one entry per lobby and binds its announcer, so only the host's client
**	forwards: it is the client with the lobby's live transcript, and a member posting the
**	same line would put it on the entry a second time. GO's system lines are skipped as
**	well - they are the server talking, not a player, and the relay's player-chat channel
**	is only for people.
**
**	The speaker's name is resolved from the roster here rather than at the call site: GO
**	sends the id and formats the text as "[Name] text", and the relay renders from + text,
**	so the name comes off the body once it is known. The bracket is the fallback for a
**	speaker who is not a member, which is how a pre-game observer's line arrives.
*/
/*
**	A lobby line this client typed itself, posted to the relay as a player line.
**
**	Direct mode used to learn lobby chat only from GO's echo of a line, and only the lobby *host*
**	forwarded it. The streamer is very often not the host (a two machine test made that plain:
**	the streaming client registered with isHost=0), so no client forwarded anything and every
**	watcher saw a silent lobby with the players chatting in it. Whoever types the line posts it:
**	one post per line, from the client that knows the text first.
*/
void GencastPostOwnLobbyChat(const std::string& message)
{
	if (message.empty())
	{
		return;
	}

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
	transport->ForwardLobbyChat(identity.displayName, message);
}

void GencastForwardLobbyChat(int64_t fromUserId, const std::string& message, bool isAnnouncement)
{
	if (isAnnouncement)
	{
		return;
	}

	// Our own line was posted the moment it was typed (GencastPostOwnLobbyChat); GO's echo of it
	// must not post it a second time.
	{
		NGMP_OnlineServices_LobbyInterface* ownLobby =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (ownLobby != nullptr)
		{
			DirectLobbyIdentity mine;

			if (ResolveDirectLobbyIdentity(mine) && mine.userId != 0 && fromUserId == mine.userId)
			{
				return;
			}
		}
	}

	NGMP_OnlineServices_LobbyInterface* lobbyInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

	if (lobbyInterface == nullptr || !lobbyInterface->IsHost())
	{
		return;
	}

	// The same gate the announce passed: direct mode, streaming switched on, a lobby, and a
	// seat. Without an announce there is no entry for the line to land on, and posting it
	// would be a request the relay refuses.
	DirectLobbyIdentity identity;

	if (!ResolveDirectLobbyIdentity(identity))
	{
		return;
	}

	std::string from;
	LobbyEntry& lobby = lobbyInterface->GetCurrentLobby();

	for (const LobbyMemberEntry& member : lobby.members)
	{
		if (member.user_id == fromUserId)
		{
			from = member.display_name;
			break;
		}
	}

	std::string text = message;

	if (from.empty() && text.size() > 2 && text[0] == '[')
	{
		const std::string::size_type close = text.find(']');

		if (close != std::string::npos && close > 1)
		{
			from = text.substr(1, close - 1);
		}
	}

	if (!from.empty())
	{
		// GO formats a member's line as "[Name] text" and, for /me, as "Name text". Both put
		// the speaker in the body, and the relay renders `from` itself, so the name comes off
		// before it travels. A normal line is always the bracketed one - a body that starts
		// with the bare name can only be an action line.
		const std::string bracketed = "[" + from + "] ";
		const std::string action = from + " ";

		if (text.rfind(bracketed, 0) == 0)
		{
			text.erase(0, bracketed.size());
		}
		else if (text.rfind(action, 0) == 0)
		{
			text.erase(0, action.size());
		}
	}

	if (text.empty())
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
	transport->ForwardLobbyChat(from, text);
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
		// Leaving the match tears the GO lobby down first, so the identity that was good at
		// announce time is the only one left. Ending the stream must not depend on still being in
		// the lobby: that is the moment the streamer is leaving it.
		if (!CachedAnnouncedMatch(identity))
		{
			return;
		}

		GencastDebugLog("gencast: end match lobby=%s match=%llu from the announce cache (%s)",
			identity.lobbyId.c_str(), (unsigned long long)identity.matchId, reason ? reason : "?");
	}

	std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

	if (transport == nullptr)
	{
		return;
	}

	transport->SetAnnounceContext(identity.playerId, identity.userId, identity.displayName,
		identity.mapName);
	transport->EndMatch(identity.lobbyId, identity.matchId, reason);

	// One end per match: the streamer's close() runs right after the recorder's stop in a normal
	// finish, and must not post a second one.
	ForgetAnnouncedMatch();
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
	// Every successful login registers, with no per-process or per-account latch. Logging out and
	// back in as somebody else is a new player to the relay and must get its own row - a once-flag
	// here left the second account unregistered for the whole run, so it never appeared in the
	// player list and could not join. The endpoint is idempotent, and its rate limit counts new
	// rows rather than calls, so a relogin only refreshes last_seen_at and the display name.
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
		// Built here rather than through GencastTransport::Create(): the mode is already known
		// (the check above), and Create() re-runs the Services reachability probe, which is a
		// blocking HEAD nobody asked for.
		std::unique_ptr<GencastTransport> transport(CreateRelayGencastTransport());

		if (transport)
		{
			transport->RegisterWatcher(userId, displayName);
		}
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


// ============================================================================
// The pregame feed's push lane
// ============================================================================

/*
**	The relay pushes the pregame lobby on GET /direct/pregame/{session} as a WebSocket: the same
**	snapshot, chat, countdown and live frames the polling lane returns in one answer. The view
**	polled at 250 ms for a while because that was cheap to write, and it read as a lobby that
**	lagged a second behind - a five second countdown is four samples, and chat lands in lumps.
**
**	The socket is the lane this client should always have used: libcurl is already linked and
**	LiveObserver drives the same calls for the match itself. The poll stays as the fallback for
**	a relay that cannot upgrade (or a socket that never connects), so a watcher is never blind.
*/

namespace
{
	std::thread s_pregameThread;
	std::atomic<bool> s_pregameRun(false);
	std::atomic<bool> s_pregameConnected(false);
	std::mutex s_pregameMutex;
	GencastLobbyStreamState s_pregameState;
	std::vector<GencastLobbyChat> s_pregameChat;
	bool s_pregameDirty = false;

	/// wss://relay/direct/pregame/{session} - the events lane's URL without its suffix.
	std::string PregameSocketUrl()
	{
		if (s_feedUrl.empty())
		{
			return std::string();
		}

		std::string path = s_feedUrl;
		const std::string suffix = "/events";

		if (path.size() > suffix.size() &&
			path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			path.erase(path.size() - suffix.size());
		}

		std::string base = NGMP_OnlineServicesManager::Settings.Relay_Direct_Url();

		while (!base.empty() && base[base.size() - 1] == '/')
		{
			base.erase(base.size() - 1);
		}

		if (base.compare(0, 8, "https://") == 0)
		{
			base = "wss://" + base.substr(8);
		}
		else if (base.compare(0, 7, "http://") == 0)
		{
			base = "ws://" + base.substr(7);
		}
		else if (base.compare(0, 6, "wss://") != 0 && base.compare(0, 5, "ws://") != 0)
		{
			return std::string();
		}

		return base + path;
	}

	void PregameApplyFrame(const std::string& text)
	{
		nlohmann::json frame = nlohmann::json::parse(text, nullptr, false);

		if (frame.is_discarded() || !frame.is_object())
		{
			return;
		}

		const std::string type = frame.value("type", std::string());

		std::lock_guard<std::mutex> lock(s_pregameMutex);

		if (type == "snapshot")
		{
			s_pregameState.fromFeed = true;
			s_pregameState.live = frame.value("phase", std::string("pregame")) == "live";
			s_pregameState.lobbyName = frame.value("lobby_name", std::string());
			s_pregameState.delaySeconds = frame.value("delay_seconds", -1);
			s_pregameState.countdownStarted = frame.value("countdown_started", false);
			s_pregameState.countdownEndsAtUnixMs = frame.value("countdown_ends_at_unix_ms", 0LL);

			if (!s_pregameState.live)
			{
				s_pregameState.startsInSeconds = -1;
			}

			s_pregameState.slots.clear();

			if (frame.contains("slots") && frame["slots"].is_array())
			{
				for (const nlohmann::json& slot : frame["slots"])
				{
					GencastLobbySlot parsed;
					parsed.slotIndex = slot.value("slot_index", -1);
					parsed.state = slot.value("state", std::string("open"));
					parsed.displayName = slot.value("display_name", std::string());
					parsed.ready = slot.value("ready", false);
					parsed.isAi = slot.value("is_ai", false);

					if (parsed.slotIndex >= 0)
					{
						s_pregameState.slots.push_back(parsed);
					}
				}
			}

			s_pregameDirty = true;
		}
		else if (type == "chat")
		{
			GencastLobbyChat line;
			line.from = frame.value("from", std::string());
			line.text = frame.value("text", std::string());
			line.kind = frame.value("kind", std::string("player"));
			line.atUnixMs = frame.value("at_unix_ms", 0LL);

			if (!line.text.empty())
			{
				s_pregameChat.push_back(line);
				s_pregameDirty = true;
			}
		}
		else if (type == "countdown")
		{
			// The transition frame says "started" or "cancelled" and carries the host's absolute
			// end, which is the only way to run the same countdown the host is running.
			const bool started = frame.value("state", std::string("cancelled")) == "started";
			s_pregameState.countdownStarted = started;
			s_pregameState.countdownEndsAtUnixMs = started
				? frame.value("ends_at_unix_ms", 0LL) : 0LL;
			s_pregameDirty = true;
		}
		else if (type == "live")
		{
			s_pregameState.live = true;
			s_pregameState.startsInSeconds = frame.value("starts_in_seconds", 0);
			const int delay = frame.value("delay_seconds", -1);

			if (delay >= 0)
			{
				s_pregameState.delaySeconds = delay;
			}

			s_pregameDirty = true;
		}
		else if (type == "error")
		{
			// lobby_gone: nothing more is coming, and the view should say so rather than wait.
			s_pregameState.live = false;
			s_pregameDirty = true;
		}
	}

	void PregameSocketLoop(std::string url)
	{
		while (s_pregameRun.load())
		{
			CURL* easy = curl_easy_init();

			if (easy == nullptr)
			{
				break;
			}

			curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
			curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);

			// wss:// needs trust anchors: this libcurl is OpenSSL, which does not read the Windows
			// store. Same bundle, same rule as LiveObserver.
			{
				std::ifstream certFile("cacert.pem");

				if (certFile.good())
				{
					certFile.close();
					curl_easy_setopt(easy, CURLOPT_CAINFO, "cacert.pem");
					curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
					curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
				}
				else
				{
					curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
					curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
				}
			}

			const CURLcode connected = curl_easy_perform(easy);

			if (connected != CURLE_OK)
			{
				curl_easy_cleanup(easy);

				GencastDebugLog("gencast: pregame socket connect failed url=%s code=%d", url.c_str(),
					(int)connected);

				for (int i = 0; i < 200 && s_pregameRun.load(); ++i)
				{
					Sleep(10);
				}

				continue;
			}

			s_pregameConnected.store(true);
			GencastDebugLog("gencast: pregame socket open url=%s", url.c_str());

			std::vector<char> buffer(65536);

			while (s_pregameRun.load())
			{
				const struct curl_ws_frame* meta = nullptr;
				size_t received = 0;
				const CURLcode rc = curl_ws_recv(easy, buffer.data(), buffer.size(), &received, &meta);

				if (rc == CURLE_AGAIN)
				{
					Sleep(10);
					continue;
				}

				if (rc != CURLE_OK)
				{
					break;
				}

				if (meta == nullptr || received == 0)
				{
					continue;
				}

				if ((meta->flags & CURLWS_PING) != 0)
				{
					size_t sent = 0;
					curl_ws_send(easy, buffer.data(), received, &sent, 0, CURLWS_PONG);
					continue;
				}

				if ((meta->flags & CURLWS_CLOSE) != 0)
				{
					break;
				}

				if ((meta->flags & CURLWS_TEXT) != 0)
				{
					PregameApplyFrame(std::string(buffer.data(), received));
				}
			}

			s_pregameConnected.store(false);
			curl_easy_cleanup(easy);

			if (!s_pregameRun.load())
			{
				break;
			}

			GencastDebugLog("gencast: pregame socket closed url=%s - reconnecting", url.c_str());

			for (int i = 0; i < 200 && s_pregameRun.load(); ++i)
			{
				Sleep(10);
			}
		}
	}
}

void GencastPregameSocketStart()
{
	GencastPregameSocketStop();

	const std::string url = PregameSocketUrl();

	if (url.empty())
	{
		return;
	}

	{
		std::lock_guard<std::mutex> lock(s_pregameMutex);
		s_pregameState = GencastLobbyStreamState();
		s_pregameChat.clear();
		s_pregameDirty = false;
	}

	s_pregameRun.store(true);
	s_pregameThread = std::thread(PregameSocketLoop, url);
}

void GencastPregameSocketStop()
{
	s_pregameRun.store(false);

	if (s_pregameThread.joinable())
	{
		s_pregameThread.join();
	}

	s_pregameConnected.store(false);

	std::lock_guard<std::mutex> lock(s_pregameMutex);
	s_pregameChat.clear();
	s_pregameDirty = false;
}

bool GencastPregameSocketConnected()
{
	return s_pregameConnected.load();
}

bool GencastPregameSocketTake(GencastLobbyStreamState& out)
{
	std::lock_guard<std::mutex> lock(s_pregameMutex);

	if (!s_pregameDirty)
	{
		return false;
	}

	out = s_pregameState;
	out.chat.swap(s_pregameChat);
	s_pregameChat.clear();
	s_pregameDirty = false;

	return true;
}
