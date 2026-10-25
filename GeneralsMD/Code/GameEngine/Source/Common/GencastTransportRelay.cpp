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
		GencastDebugLog("relay: %s %s -> status=%ld bytes=%zu%s",
			postBody != nullptr ? "POST" : "GET", url.c_str(), out.status, out.body.size(),
			rc == CURLE_OK ? "" : " (curl failed)");

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

	class RelayGencastTransport : public GencastTransport
	{
	public:
		RelayGencastTransport()
		{
			// Puts the client identity and the relay it is talking to in the log that ships to the
			// relay, so the tester's file and the server's agree on who said what.
			GencastDebugLog("gencast: direct transport relay=%s client=%s instance=%d",
				BaseUrl().c_str(), GencastClientId(), GencastClientInstance());
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
			const std::string& password) override
		{
			GencastAttemptResult result;

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["password"] = password;

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

		void RegisterStream(const std::string& lobbyId, GencastRegisterCallback done) override
		{
			GencastStreamRegistration registration;

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["player_id"] = m_playerId;

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
			// The streamer registers when the match starts (LiveStreamer::registerForGame), not
			// while sitting in the lobby, so the relay's entry is live from its first breath.
			request["state"] = "live";

			// Same reason as the join request: map and player names come from game data and may
			// not be valid UTF-8, and dump() throws on that. This runs on the streamer's network
			// thread, where an escaping exception terminates the process.
			std::string postBody;

			try
			{
				postBody = request.dump();
			}
			catch (const nlohmann::json::exception&)
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

			GencastDebugLog("gencast: announce lobby=%s player=%ld -> key=%zu chars url=%s%s",
				lobbyId.c_str(), m_playerId, registration.streamKey.size(),
				registration.streamUrl.c_str(),
				registration.success ? "" : " (rejected)");

			if (!registration.success && registration.errorMessage.empty())
			{
				registration.errorMessage = "The relay returned no stream address.";
			}

			done(registration);
		}

		// The direct relay has no lobby-subscription channel: the watch list is polled and the
		// session feed is read per session, so there is nothing to subscribe to here.
		void SubscribeLobbyUpdates(const std::string&) override {}
		void UnsubscribeLobbyUpdates() override {}

	private:
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
