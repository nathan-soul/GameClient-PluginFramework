#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"

#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"

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

		// The relay answers with a rooted path (/direct/watch/{id}); the host is its base URL.
		return ToWebSocketUrl(BaseUrl()) + path;
	}

	class RelayGencastTransport : public GencastTransport
	{
	public:
		EGencastMode Mode() const override { return EGencastMode::Direct; }

		bool IsAvailable() const override { return !BaseUrl().empty(); }

		void SetAnnounceContext(long playerId, const std::string& mapName,
			const std::string& playersCsv) override
		{
			m_playerId = playerId;
			m_mapName = mapName;
			m_playersCsv = playersCsv;
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

			done(true, entries);
		}

		void RequestWatchSession(const std::string& lobbyId, const std::string& password,
			GencastSessionCallback done) override
		{
			GencastWatchSession session;

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["password"] = password;

			HttpResponse response;

			if (!Perform(BaseUrl() + "/direct/join", request.dump().c_str(), response))
			{
				session.errorMessage = "The relay did not answer.";
				done(session);
				return;
			}

			session.httpStatus = static_cast<int>(response.status);

			// 401 is "wrong password" and the caller must say exactly that, never "signed out".
			if (response.status != 200)
			{
				session.errorMessage = response.status == 401
					? "Wrong password."
					: "That lobby is no longer watchable.";
				done(session);
				return;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);
				session.sessionId = body.value("session_id", "");
				const std::string path = body.value("stream_url", "");

				if (!path.empty())
				{
					session.streamUrl = path;
					session.wsUrl = ToWebSocketUrl(path);
				}
			}
			catch (const nlohmann::json::exception&)
			{
				session.errorMessage = "The relay sent something unreadable.";
				done(session);
				return;
			}

			session.success = !session.wsUrl.empty();
			done(session);
		}

		void RegisterStream(const std::string& lobbyId, GencastRegisterCallback done) override
		{
			GencastStreamRegistration registration;

			nlohmann::json players = nlohmann::json::array();

			if (!m_playersCsv.empty())
			{
				size_t start = 0;

				while (start <= m_playersCsv.size())
				{
					const size_t comma = m_playersCsv.find(',', start);

					std::string name = m_playersCsv.substr(start,
						comma == std::string::npos ? std::string::npos : comma - start);

					while (!name.empty() && name[0] == ' ')
					{
						name.erase(0, 1);
					}

					if (!name.empty())
					{
						players.push_back(name);
					}

					if (comma == std::string::npos)
					{
						break;
					}

					start = comma + 1;
				}
			}

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["player_id"] = m_playerId;
			request["map_name"] = m_mapName;
			request["players"] = players;
			request["slots"] = nlohmann::json::array();
			request["state"] = "lobby";

			HttpResponse response;

			if (!Perform(BaseUrl() + "/direct/announce", request.dump().c_str(), response))
			{
				registration.errorMessage = "The relay did not answer.";
				done(registration);
				return;
			}

			if (response.status != 200)
			{
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

				if (!path.empty())
				{
					registration.streamUrl = path;
				}
			}
			catch (const nlohmann::json::exception&)
			{
				registration.errorMessage = "The relay sent something unreadable.";
				done(registration);
				return;
			}

			registration.success = !registration.streamKey.empty();
			done(registration);
		}

		// The direct relay has no lobby-subscription channel: the watch list is polled and the
		// session feed is read per session, so there is nothing to subscribe to here.
		void SubscribeLobbyUpdates(const std::string&) override {}
		void UnsubscribeLobbyUpdates() override {}

	private:
		long m_playerId = 0;
		std::string m_mapName;
		std::string m_playersCsv;
	};
}

GencastTransport* CreateRelayGencastTransport()
{
	return new RelayGencastTransport();
}

#endif // GENERALS_ONLINE
