#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"
#include "Common/GlobalData.h"
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
	const std::string& password, const std::string& matchId, GencastSessionCallback done,
	GencastCancelPredicate cancelled)
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

		const GencastAttemptResult attempt = AttemptWatchSession(lobbyId, password, matchId);

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
		if (!NGMP_OnlineServicesManager::Settings.Relay_Direct() ||
			NGMP_OnlineServicesManager::Settings.Relay_Direct_Url().empty())
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

#endif // GENERALS_ONLINE
