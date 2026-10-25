#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"
#include "Common/GencastClientLog.h"

#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
// The announce carries the lobby roster, so the transport reads the lobby the player is in.
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include <curl/curl.h>

#include <mutex>
#include <thread>

/*
**	Direct transport: the client talks to the relay itself.
**
**	This is the counterpart of what Services does today for streamers and watchers, reduced to
**	the three calls that matter — list the lobbies, ask for a watch session, announce a stream.
**	The relay owns access control, so there is no token, no ticket and no priority here: the
**	password is the gate and a wrong one comes back as 401.
**
**	Requests are synchronous, like liveServicesRequest on the Services path, and the callback
**	runs on the caller's thread. A 5-second timeout keeps a dead relay from freezing the menu.
*/

namespace
{
	const long kTimeoutSeconds = 5;

	/*
	**	The last announce body this client sent for a lobby, and the lock that guards it.
	**
	**	A lobby is refreshed far more often than it changes, and every announce is an HTTP
	**	request, so identical bodies are dropped instead of re-sent. The whole body is kept, not a
	**	digest of the lobby: it is exactly what the relay stored, so there is no second projection
	**	of the lobby that could fall out of step with the first.
	**
	**	One lobby at a time per client, so a single slot is enough; a body carries the lobby id and
	**	so always differs for a different lobby. The lock is for the game thread (the only caller
	**	today) plus any future one, not for the worker: the worker only ever reads the copy it was
	**	given.
	*/
	std::mutex s_announceMutex;
	std::string s_lastAnnounceBody;

	size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		const size_t bytes = size * nmemb;
		static_cast<std::string*>(userdata)->append(ptr, bytes);
		return bytes;
	}

	struct HttpResponse
	{
		long status = 0;
		std::string body;
	};

bool Perform(const std::string& url, const char* postBody, HttpResponse& out)
	{
		CURL* curl = curl_easy_init();

		if (curl == nullptr)
		{
			return false;
		}

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
		// The game is multi-threaded by the time these run (observer and streamer each own a
		// network thread). Without NOSIGNAL, libcurl resolves timeouts with SIGALRM, which can
		// land in another thread's context and take the process down.
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);

		// This libcurl is OpenSSL-backed, and OpenSSL has no trust anchors of its own on
		// Windows: without this every https call fails verification while the same URL
		// answers from curl.exe, which uses Schannel. NATIVE_CA makes it read the Windows
		// certificate store - the same option, and for the same reason, as the GO HTTP
		// client in this tree (OnlineServices/HTTP/HTTPRequest.cpp).
		curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);

		curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		if (postBody != nullptr)
		{
			curl_easy_setopt(curl, CURLOPT_POST, 1L);
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody);
		}

		const CURLcode rc = curl_easy_perform(curl);

		if (rc == CURLE_OK)
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		// One line per relay call: what we asked, what came back. This is the first thing to look
		// at when direct mode does not behave, and it ships to the relay's own log.
		// The curl reason is included: "curl failed" alone says nothing about whether it was DNS,
		// a refused connection or certificate verification, and those need different fixes.
		GencastDebugLog("relay: %s %s -> status=%ld bytes=%zu%s%s%s",
			postBody != nullptr ? "POST" : "GET", url.c_str(), out.status, out.body.size(),
			rc == CURLE_OK ? "" : " (curl failed: ", rc == CURLE_OK ? "" : curl_easy_strerror(rc),
			rc == CURLE_OK ? "" : ")");

		return rc == CURLE_OK;
	}

	/// The relay's base URL without a trailing slash, so paths concatenate predictably.
	std::string BaseUrl()
	{
		std::string url = NGMP_OnlineServicesManager::Settings.Relay_Direct_Url();

		while (!url.empty() && url[url.size() - 1] == '/')
		{
			url.erase(url.size() - 1);
		}

		return url;
	}

	/// http://host:port/direct/stream/k -> ws://host:port/direct/stream/k
	///
	/// A rooted path ("/direct/watch/{id}") gets the relay's host in front of it. The base is
	/// resolved here rather than by calling this again with BaseUrl(): that recursion never
	/// terminates when the base is empty or carries no scheme, which is a stack overflow crash
	/// instead of a bad URL. An unusable base returns an empty string and the caller reports a
	/// missing address.
	std::string ToWebSocketUrl(const std::string& path)
	{
		if (path.rfind("ws://", 0) == 0 || path.rfind("wss://", 0) == 0)
		{
			return path;
		}

		if (path.rfind("http://", 0) == 0)
		{
			return "ws://" + path.substr(7);
		}

		if (path.rfind("https://", 0) == 0)
		{
			return "wss://" + path.substr(8);
		}

		const std::string base = BaseUrl();
		std::string socketBase;

		if (base.rfind("https://", 0) == 0)
		{
			socketBase = "wss://" + base.substr(8);
		}
		else if (base.rfind("http://", 0) == 0)
		{
			socketBase = "ws://" + base.substr(7);
		}
		else if (base.rfind("ws://", 0) == 0 || base.rfind("wss://", 0) == 0)
		{
			socketBase = base;
		}
		else
		{
			return std::string();
		}

		// The relay always answers with a rooted path, but a relative one must not be glued to
		// the host as "hostdirect/...".
		if (!path.empty() && path[0] != '/')
		{
			return socketBase + "/" + path;
		}

		return socketBase + path;
	}

	/// The relay's slot state is a name, not the engine's enum ordinal: the enum's numbering is
	/// a client implementation detail, while this string ends up in the watch list.
	const char* SlotStateName(SlotState state)
	{
		switch (state)
		{
		case SLOT_OPEN: return "open";
		case SLOT_CLOSED: return "closed";
		case SLOT_EASY_AI: return "easy_ai";
		case SLOT_MED_AI: return "medium_ai";
		case SLOT_BRUTAL_AI: return "brutal_ai";
		case SLOT_PLAYER: return "player";
		}

		return "open";
	}

	bool IsAiSlotState(SlotState state)
	{
		return state == SLOT_EASY_AI || state == SLOT_MED_AI || state == SLOT_BRUTAL_AI;
	}

	/// The roster as the lobby currently shows it.
	///
	/// Unseated members (SLOT_INDEX_NONE) are left out: they have no seat to report, and their
	/// marker is not a slot index. Members the host has not assigned yet keep whatever state
	/// they carry (open/closed), so a viewer sees the same board the players do.
	nlohmann::json SlotsFromCurrentLobby()
	{
		nlohmann::json slots = nlohmann::json::array();

		NGMP_OnlineServices_LobbyInterface* lobby =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (lobby == nullptr)
		{
			return slots;
		}

		for (const LobbyMemberEntry& member : lobby->GetCurrentLobby().members)
		{
			if (member.m_SlotIndex == LobbyMemberEntry::SLOT_INDEX_NONE)
			{
				continue;
			}

			const SlotState state = static_cast<SlotState>(member.m_SlotState);

			nlohmann::json slot;
			slot["slot_index"] = member.m_SlotIndex;
			slot["state"] = SlotStateName(state);
			slot["team"] = member.team;
			slot["color"] = member.color;
			slot["startpos"] = member.startpos;
			slot["display_name"] = member.display_name;
			slot["is_ai"] = IsAiSlotState(state);
			slots.push_back(slot);
		}

		return slots;
	}

	/// GO's identity for the match in the lobby this client is in, 0 when GO has none.
	///
	/// The relay keys a watch entry on this, not on the lobby: a finished match in its post-game
	/// hold and the next match in the same lobby are two entries. GO knows the id from the moment
	/// the lobby exists (UpdateRoomDataCache stores it before the announce runs), so a real lobby
	/// always answers with one; 0 is a client bug and is reported where it is used.
	uint64_t CurrentMatchId()
	{
		NGMP_OnlineServices_LobbyInterface* lobby =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		return lobby != nullptr ? lobby->GetCurrentMatchID() : 0;
	}

	class RelayGencastTransport : public GencastTransport
	{
	public:
		RelayGencastTransport()
		{
			// Puts the client identity and the relay it is talking to in the log that ships to the
			// relay, so the tester's file and the server's agree on who said what. The libcurl
			// banner names the TLS backend: a static build without a CA store fails every https
			// call while Windows' own stack succeeds, and that is invisible from the URL alone.
			GencastDebugLog("gencast: direct transport relay=%s client=%s instance=%d curl=%s",
				BaseUrl().c_str(), GencastClientId(), GencastClientInstance(), curl_version());
		}

		EGencastMode Mode() const override { return EGencastMode::Direct; }

		bool IsAvailable() const override { return !BaseUrl().empty(); }

		void SetAnnounceContext(long playerId, long userId, const std::string& displayName,
			const std::string& mapName) override
		{
			m_playerId = playerId;
			m_userId = userId;
			m_displayName = displayName;
			m_mapName = mapName;
		}

		void FetchWatchList(GencastWatchListCallback done) override
		{
			std::vector<GencastWatchEntry> entries;
			HttpResponse response;

			if (!Perform(BaseUrl() + "/direct/lobbies", nullptr, response) || response.status != 200)
			{
				done(false, entries);
				return;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);
				const nlohmann::json& lobbies = body.at("lobbies");

				for (const nlohmann::json& lobby : lobbies)
				{
					GencastWatchEntry entry;
					entry.lobbyId = lobby.value("lobby_id", "");
					// A lobby can list a finished match next to the next one, so the row has to
					// say which game it is. The relay prints it as a string; a number is read
					// too rather than failing the whole list over the type of one field.
					if (lobby.contains("match_id"))
					{
						const nlohmann::json& matchId = lobby["match_id"];

						if (matchId.is_string())
						{
							entry.matchId = matchId.get<std::string>();
						}
						else if (matchId.is_number())
						{
							entry.matchId = matchId.dump();
						}
					}
					entry.mapName = lobby.value("map_name", "");
					entry.state = lobby.value("state", "");
					entry.hasPassword = lobby.value("has_password", false);

					if (lobby.contains("players"))
					{
						entry.playersJson = lobby["players"].dump();
					}

					if (!entry.lobbyId.empty())
					{
						entries.push_back(entry);
					}
				}
			}
			catch (const nlohmann::json::exception&)
			{
				done(false, std::vector<GencastWatchEntry>());
				return;
			}

			GencastDebugLog("gencast: watch list -> %zu lobbies", entries.size());

			done(true, entries);
		}

		GencastAttemptResult AttemptWatchSession(const std::string& lobbyId,
			const std::string& password, const std::string& matchId) override
		{
			GencastAttemptResult result;

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["password"] = password;

			// Which match of that lobby to watch. Without it the relay picks the newest game that
			// has not ended, which is the wrong one when a finished match is still in its post-game
			// hold beside the next: a viewer who clicked "ended" would be handed the new match, or
			// the 404 that means "no stream yet". Empty for callers that only know the lobby.
			if (!matchId.empty())
			{
				request["match_id"] = matchId;
			}

			// dump() throws on invalid UTF-8, and the password comes straight from a text
			// entry. Serialising outside a try would let that exception escape into the
			// observer's network thread, where nothing catches it and the process dies.
			std::string postBody;

			try
			{
				postBody = request.dump();
			}
			catch (const nlohmann::json::exception&)
			{
				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The password could not be encoded.";
				return result;
			}

			HttpResponse response;

			if (!Perform(BaseUrl() + "/direct/join", postBody.c_str(), response))
			{
				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The relay did not answer.";
				return result;
			}

			result.httpStatus = (int)response.status;

			// 401 is a real answer and must never be retried or reported as signed out.
			if (response.status == 401)
			{
				GencastDebugLog("gencast: join lobby=%s -> wrong password", lobbyId.c_str());

				result.kind = EGencastAttempt::WrongPassword;
				result.errorMessage = "Wrong password.";
				return result;
			}

			// The streamer may not have announced yet - the shared policy retries this window.
			if (response.status == 404)
			{
				GencastDebugLog("gencast: join lobby=%s -> not announced yet", lobbyId.c_str());

				result.kind = EGencastAttempt::NotFound;
				return result;
			}

			if (response.status != 200)
			{
				GencastDebugLog("gencast: join lobby=%s -> refused status=%ld",
					lobbyId.c_str(), response.status);

				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The relay refused the session.";
				return result;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);
				result.sessionId = body.value("session_id", "");
				result.sessionUrl = body.value("ws_url", "");
				result.streamUrl = body.value("stream_url", "");

				if (!result.streamUrl.empty())
				{
					result.wsUrl = ToWebSocketUrl(result.streamUrl);
				}
			}
			catch (const nlohmann::json::exception&)
			{
				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The relay sent something unreadable.";
				return result;
			}

			if (result.wsUrl.empty())
			{
				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The relay returned no stream address.";
				return result;
			}

			GencastDebugLog("gencast: join lobby=%s -> session=%s ws=%s",
				lobbyId.c_str(), result.sessionId.c_str(), result.wsUrl.c_str());

			result.kind = EGencastAttempt::Success;
			return result;
		}

		void RegisterStream(const std::string& lobbyId, EGencastStreamState state,
			GencastRegisterCallback done) override
		{
			GencastStreamRegistration registration;

			const std::string postBody = BuildAnnounceBody(lobbyId, state);

			// Same reason as the join request: map and player names come from game data and may
			// not be valid UTF-8, and dump() throws on that. This runs on the streamer's network
			// thread, where an escaping exception terminates the process; an empty body is
			// BuildAnnounceBody reporting exactly that.
			if (postBody.empty())
			{
				registration.errorMessage = "The lobby details could not be encoded.";
				done(registration);
				return;
			}

			HttpResponse response;

			if (!Perform(BaseUrl() + "/direct/announce", postBody.c_str(), response))
			{
				registration.errorMessage = "The relay did not answer.";
				done(registration);
				return;
			}

			if (response.status != 200)
			{
				GencastDebugLog("gencast: announce lobby=%s player=%ld -> status=%ld",
					lobbyId.c_str(), m_playerId, response.status);

				registration.errorMessage = response.status == 404
					? "Direct watch is closed on that relay."
					: "The relay refused the announcement.";
				done(registration);
				return;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);

				// The relay answers {"streaming_allowed": false} when direct watch is disabled.
				if (!body.value("streaming_allowed", true))
				{
					registration.errorMessage = "Direct watch is closed on that relay.";
					done(registration);
					return;
				}

				registration.streamKey = body.value("stream_key", "");
				const std::string path = body.value("stream_url", "");

				// The relay answers the announce with a rooted path
				// ("/direct/stream/{key}"), and the streamer hands this straight to CURLOPT_URL,
				// which needs a scheme. Passing the path through is a connection that never
				// opens.
				if (!path.empty())
				{
					registration.streamUrl = ToWebSocketUrl(path);
				}
			}
			catch (const nlohmann::json::exception&)
			{
				registration.errorMessage = "The relay sent something unreadable.";
				done(registration);
				return;
			}

			registration.success = !registration.streamKey.empty() && !registration.streamUrl.empty();

			if (registration.success)
			{
				// This path is one of the two that announce, so the change comparison has to learn
				// about it: the phase just moved to live, and a lobby that returns to pregame after
				// a match would otherwise compare equal to the pregame body from before it and be
				// dropped, leaving the relay reporting a live lobby with no match behind it.
				std::scoped_lock<std::mutex> lock(s_announceMutex);
				s_lastAnnounceBody = postBody;
			}

			GencastDebugLog("gencast: announce lobby=%s player=%ld state=%s -> key=%zu chars url=%s%s",
				lobbyId.c_str(), m_playerId, GencastStreamStateName(state),
				registration.streamKey.size(), registration.streamUrl.c_str(),
				registration.success ? "" : " (rejected)");

			// The phase change to live, in the log the rest of the client writes to. This runs on
			// the streamer's network thread; NetworkLog takes its own lock.
			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[GENCAST] relay announce lobby=%s state=%s streaming=%s",
				lobbyId.c_str(), GencastStreamStateName(state),
				registration.success ? "yes" : "no");

			if (!registration.success && registration.errorMessage.empty())
			{
				registration.errorMessage = "The relay returned no stream address.";
			}

			done(registration);
		}

		/// Fire-and-forget announce, used while the players are still in the lobby.
		///
		/// The body is built here, on the caller's thread, and only the finished request goes to
		/// the worker: the lobby is a live engine structure that the game thread rewrites on every
		/// update, and reading it from a worker would be a data race for no gain.
		void AnnounceLobby(const std::string& lobbyId, EGencastStreamState state) override
		{
			const std::string body = BuildAnnounceBody(lobbyId, state);

			if (body.empty())
			{
				GencastDebugLog("gencast: lobby announce lobby=%s skipped, details unencodable",
					lobbyId.c_str());
				return;
			}

			// The body is the whole of what the relay would store, so comparing it with the last
			// one is an exact "did anything change" test: ready flags, chat and the map-name
			// spelling of the lobby cost no request at all, while a new seat, team, colour, map or
			// phase does. (A body carries the lobby id, so another lobby always differs.)
			{
				std::scoped_lock<std::mutex> lock(s_announceMutex);

				if (body == s_lastAnnounceBody)
				{
					return;
				}

				s_lastAnnounceBody = body;
			}

			// Also in GeneralsOnline.log: this is the line that says the relay was told about the
			// lobby before the match, which is the whole point of announcing from here.
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[GENCAST] relay announce lobby=%s state=%s",
				lobbyId.c_str(), GencastStreamStateName(state));

			// Perform() is synchronous with the same 5 s timeout as the rest of the transport, and
			// this is reached from a lobby refresh - the game thread. Sending inline would spend
			// that timeout inside a frame whenever the relay is slow, so the request goes to a
			// worker that holds nothing but strings (the transport may be deleted under it).
			const std::string url = BaseUrl() + "/direct/announce";
			const std::string stateName = GencastStreamStateName(state);

			std::thread([url, body, lobbyId, stateName]()
				{
					HttpResponse response;
					const bool answered = Perform(url, body.c_str(), response);

					if (!answered || response.status != 200)
					{
						// Nothing landed, so forget the body: the next lobby refresh has to try
						// again, or one failed request would leave the lobby unannounced for as
						// long as it does not change. Only if it is still the newest body, so a
						// later announce is not thrown away.
						{
							std::scoped_lock<std::mutex> lock(s_announceMutex);

							if (s_lastAnnounceBody == body)
							{
								s_lastAnnounceBody.clear();
							}
						}

						GencastDebugLog("gencast: lobby announce lobby=%s state=%s -> %s",
							lobbyId.c_str(), stateName.c_str(),
							answered ? "refused" : "relay unreachable");
						return;
					}

					GencastDebugLog("gencast: lobby announce lobby=%s state=%s -> status=%ld",
						lobbyId.c_str(), stateName.c_str(), response.status);
				}).detach();
		}

		// The direct relay has no lobby-subscription channel: the watch list is polled and the
		// session feed is read per session, so there is nothing to subscribe to here.
		void SubscribeLobbyUpdates(const std::string&) override {}
		void UnsubscribeLobbyUpdates() override {}

		/// "This player is no longer in that lobby" - the relay's half of GO's DELETE {Lobby}/{id}.
		///
		/// Reached from LeaveCurrentLobby, which is the only leave funnel: host-disband and
		/// member-leave both come through here, and the relay - which holds the announcer binding
		/// - decides whether that drops the watch entry or leaves another streamer's alone.
		void UnannounceLobby(const std::string& lobbyId, uint64_t matchId) override
		{
			if (m_playerId <= 0)
			{
				// The relay rejects player id 0 outright, and an announce under no player id
				// never created an entry to drop.
				GencastDebugLog("gencast: unannounce lobby=%s skipped, no player id", lobbyId.c_str());
				return;
			}

			const std::string body = BuildLifecycleBody(lobbyId, matchId, nullptr);

			if (body.empty())
			{
				GencastDebugLog("gencast: unannounce lobby=%s skipped, details unencodable",
					lobbyId.c_str());
				return;
			}

			{
				// The relay no longer holds an entry for this lobby, so the announce that follows
				// a re-join must go out even if it is byte for byte the last one: without this,
				// leaving a lobby and coming back to it would leave it unannounced for as long as
				// nothing about it changes.
				std::scoped_lock<std::mutex> lock(s_announceMutex);
				s_lastAnnounceBody.clear();
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[GENCAST] relay unannounce lobby=%s match=%llu",
				lobbyId.c_str(), (unsigned long long)matchId);

			PostLifecycle("/direct/unannounce", lobbyId, matchId, body);
		}

		/// "The match is over" - starts the relay's post-game hold for this match.
		///
		/// Announcer-only at the relay, which answers 200 and does nothing for anyone else, so a
		/// client that was not the one streaming it cannot end somebody else's game.
		void EndMatch(const std::string& lobbyId, uint64_t matchId, const char* reason) override
		{
			if (m_playerId <= 0)
			{
				GencastDebugLog("gencast: end lobby=%s skipped, no player id", lobbyId.c_str());
				return;
			}

			const std::string body = BuildLifecycleBody(lobbyId, matchId, reason);

			if (body.empty())
			{
				GencastDebugLog("gencast: end lobby=%s skipped, details unencodable", lobbyId.c_str());
				return;
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"[GENCAST] relay end lobby=%s match=%llu reason=%s", lobbyId.c_str(),
				(unsigned long long)matchId, reason != nullptr ? reason : "");

			PostLifecycle("/direct/end", lobbyId, matchId, body);
		}

	private:
		/// The body /direct/unannounce and /direct/end share: which game, and who is saying so.
		///
		/// Both name the game the same way - (lobby_id, match_id) - because that pair is what the
		/// relay resolves a lifecycle call to. reason is /direct/end's, and null for unannounce,
		/// where the relay has nothing to explain.
		std::string BuildLifecycleBody(const std::string& lobbyId, uint64_t matchId,
			const char* reason)
		{
			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["player_id"] = m_playerId;
			// A uint64 as a string: JSON numbers are doubles to some readers, and GO's match ids
			// are handed out by a service this client does not control.
			request["match_id"] = std::to_string(matchId);

			if (reason != nullptr)
			{
				request["reason"] = reason;
			}

			try
			{
				return request.dump();
			}
			catch (const nlohmann::json::exception&)
			{
				return std::string();
			}
		}

		/// POST one lifecycle report on a worker and forget it.
		///
		/// Both callers run on the game thread (leaving a lobby, the end of a match), and
		/// Perform() is synchronous with a five second timeout: sending inline would spend that
		/// timeout in a frame whenever the relay is slow. The worker holds nothing but strings, so
		/// the transport may be deleted under it - the same contract as the announce.
		void PostLifecycle(const char* path, const std::string& lobbyId, uint64_t matchId,
			const std::string& body)
		{
			const std::string url = BaseUrl() + path;
			const std::string pathCopy = path;

			std::thread([url, pathCopy, body, lobbyId, matchId]()
				{
					HttpResponse response;
					const bool answered = Perform(url, body.c_str(), response);

					if (!answered || response.status != 200)
					{
						GencastDebugLog("gencast: %s lobby=%s match=%llu -> %s status=%ld",
							pathCopy.c_str(), lobbyId.c_str(), (unsigned long long)matchId,
							answered ? "refused" : "relay unreachable", response.status);
						return;
					}

					GencastDebugLog("gencast: %s lobby=%s match=%llu -> status=%ld",
						pathCopy.c_str(), lobbyId.c_str(), (unsigned long long)matchId,
						response.status);
				}).detach();
		}

		/// The /direct/announce body for the lobby as it is right now.
		///
		/// Shared by both announce paths so they cannot disagree about what a lobby looks like:
		/// RegisterStream when a stream is starting, AnnounceLobby whenever the lobby changes.
		/// Empty means the details could not be encoded (invalid UTF-8 in a name, which dump()
		/// throws on), and the caller reports or logs it.
		std::string BuildAnnounceBody(const std::string& lobbyId, EGencastStreamState state)
		{
			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["player_id"] = m_playerId;

			// The entry's identity at the relay. A lobby can list a finished match beside the
			// next one, so lobby_id alone is metadata now; match_id is what the relay keys on,
			// unannounce and end target, and the watch list names a row by.
			//
			// GO has this from the moment the lobby exists, so 0 means the client asked before
			// GO said (or a GO that never sends MatchID). It is sent rather than omitted: the
			// relay then keys the entry on the lobby id - the game stays watchable - and logs
			// the anomaly on both sides instead of dropping the entry.
			const uint64_t matchId = CurrentMatchId();

			if (matchId == 0)
			{
				GencastDebugLog("gencast: announce lobby=%s has no match id (GetCurrentMatchID=0), "
					"the relay will key this entry on the lobby id",
					lobbyId.c_str());
			}

			request["match_id"] = std::to_string(matchId);

			// The relay keys its player record on the Services user id and only falls back to
			// the wire player id, so an unknown id is left out rather than sent as -1.
			if (m_userId > 0)
			{
				request["user_id"] = m_userId;
			}

			request["display_name"] = m_displayName;
			request["map_name"] = m_mapName;
			// The roster now travels in slots; players is kept as an empty array because the
			// relay still carries the field, and nothing else fills it any more.
			request["players"] = nlohmann::json::array();
			request["slots"] = SlotsFromCurrentLobby();
			// The phase is the caller's, never assumed here: this body is sent both from the
			// lobby (pregame) and from the streamer at match start (live).
			request["state"] = GencastStreamStateName(state);

			try
			{
				return request.dump();
			}
			catch (const nlohmann::json::exception&)
			{
				return std::string();
			}
		}

		long m_playerId = 0;
		// The Services user id, or <= 0 when the client is not signed in. The relay keys its
		// player record on this one and falls back to m_playerId for older clients.
		long m_userId = 0;
		std::string m_displayName;
		std::string m_mapName;
	};
}

GencastTransport* CreateRelayGencastTransport()
{
	return new RelayGencastTransport();
}

#endif // GENERALS_ONLINE
