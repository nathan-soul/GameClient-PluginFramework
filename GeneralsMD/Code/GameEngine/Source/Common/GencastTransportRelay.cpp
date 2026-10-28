#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"
#include "Common/GencastClientLog.h"
#include "GameClient/GameText.h"
#include "GameClient/GencastCountdownMirror.h"
#include "GameClient/GencastAnnounceIdentity.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"

#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
// The announce carries the lobby roster, so the transport reads the lobby the player is in.
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
// ... and the host's countdown, which is client state on the NGMP game, not on the lobby.
#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
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
			// Whether the player in this seat has pressed Ready. Display data for the relay's
			// pregame lobby and its admin panel; nothing acts on it, the host starts the match.
			slot["ready"] = member.m_bIsReady;
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

	/// GO's id for the lobby this client is in, as decimal text, empty when it is in none.
	///
	/// The announce is handed this by its callers; the chat forward is not, so it reads it here
	/// rather than taking a lobby id it would only pass through. Same spelling as the announce
	/// sends, because both name the same entry at the relay.
	std::string CurrentLobbyId()
	{
		NGMP_OnlineServices_LobbyInterface* lobby =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (lobby == nullptr || !lobby->IsInLobby())
		{
			return std::string();
		}

		char lobbyId[32];
		snprintf(lobbyId, sizeof(lobbyId), "%lld", (long long)lobby->GetCurrentLobby().lobbyID);
		return lobbyId;
	}

	// A member's client mirrors the host's countdown (see GencastCountdownMirror.h).
	static std::mutex s_gencastMirrorCountdownMutex;
	static GencastMirrorCountdown s_gencastMirrorCountdown;

	static int64_t GencastWallClockMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	/// The lobby's readiness folded into one number. A member going un-ready is the same event that
	/// stops the host's countdown, so the mirror watches this rather than the countdown it cannot see.
	static bool GencastCurrentLobbyReadySignature(unsigned& outSignature)
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

		if (pLobbyInterface == nullptr || pLobbyInterface->GetCurrentGame() == nullptr)
		{
			return false;
		}

		unsigned signature = 0;

		for (const LobbyMemberEntry& member : pLobbyInterface->GetCurrentLobby().members)
		{
			signature = GencastReadySignatureStep(signature, member.m_SlotIndex, member.m_bIsReady);
		}

		outSignature = signature;

		return true;
	}

	static void GencastClearMirrorCountdown()
	{
		std::lock_guard<std::mutex> lock(s_gencastMirrorCountdownMutex);

		s_gencastMirrorCountdown.clear();
	}

	/// The mirrored end, when this client has one that still applies. Drops it when readiness moved,
	/// because that is the same lobby change that stopped the host's countdown.
	static bool GencastMirrorCountdownEndsAt(int64_t& outEndsAtUnixMs)
	{
		std::lock_guard<std::mutex> lock(s_gencastMirrorCountdownMutex);

		unsigned signature = 0;

		if (GencastCurrentLobbyReadySignature(signature) &&
			s_gencastMirrorCountdown.cancelIfReadyChanged(signature))
		{
			GencastDebugLog("gencast: countdown mirror cancelled (the lobby went un-ready)");
		}

		if (s_gencastMirrorCountdown.armed && !s_gencastMirrorCountdown.active(GencastWallClockMs()))
		{
			s_gencastMirrorCountdown.clear();

			GencastDebugLog("gencast: countdown mirror expired");
		}

		if (!s_gencastMirrorCountdown.active(GencastWallClockMs()))
		{
			return false;
		}

		outEndsAtUnixMs = s_gencastMirrorCountdown.endsAtUnixMs;

		return true;
	}

	/// Arms the mirror, or cancels it, from a lobby announcement. Called on every announcement a
	/// member receives, which is why it says what it decided.
	void GencastNoteLobbyAnnouncementInternal(const std::string& message, bool isAnnouncement)
	{
		if (!isAnnouncement || message.empty())
		{
			return;
		}

		NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		NGMPGame* pGame = pLobbyInterface != nullptr ? pLobbyInterface->GetCurrentGame() : nullptr;

		// The host runs the real countdown and publishes it from its own state; a mirror there would
		// be a second opinion about the same thing.
		if (pGame == nullptr || pGame->amIHost())
		{
			return;
		}

		int duration = pGame->GetTotalCountdownDuration();

		if (duration <= 0)
		{
			duration = GencastMirrorCountdown::kDefaultDurationSeconds;
		}

		// The host builds its line from these two strings, so the same build recognises it whatever
		// the language is set to - both sides read the same game text.
		UnicodeString plural;
		plural.format(TheGameText->fetch("LAN:GameStartTimerPlural"), duration);

		UnicodeString singular;
		singular.format(TheGameText->fetch("LAN:GameStartTimerSingular"), 1);

		const UnicodeString incoming(from_utf8(message).c_str());

		const bool startLine = (incoming.compare(plural) == 0 || incoming.compare(singular) == 0);

		unsigned signature = 0;
		GencastCurrentLobbyReadySignature(signature);

		bool announce = false;

		{
			std::lock_guard<std::mutex> lock(s_gencastMirrorCountdownMutex);

			if (startLine)
			{
				s_gencastMirrorCountdown.arm(GencastWallClockMs(), duration, signature);
				announce = true;

				GencastDebugLog("gencast: countdown mirror armed via=announcement ends_in_ms=%d "
					"(this client is not the lobby host)", duration * 1000);
			}
		}

		if (announce)
		{
			GencastAnnounceCurrentLobby();
		}
	}

	/// The same mirror, armed from GO's own START_GAME_COUNTDOWN_STARTED. It is sent by the host and
	/// may or may not be fanned out to lobby members; when it is, it is the cleaner of the two
	/// signals and the log says which one fired.
	void GencastNoteHostCountdownStartedInternal()
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		NGMPGame* pGame = pLobbyInterface != nullptr ? pLobbyInterface->GetCurrentGame() : nullptr;

		if (pGame == nullptr || pGame->amIHost())
		{
			return;
		}

		unsigned signature = 0;
		GencastCurrentLobbyReadySignature(signature);

		{
			std::lock_guard<std::mutex> lock(s_gencastMirrorCountdownMutex);

			s_gencastMirrorCountdown.arm(GencastWallClockMs(), pGame->GetTotalCountdownDuration(), signature);
		}

		GencastDebugLog("gencast: countdown mirror armed via=ws");

		GencastAnnounceCurrentLobby();
	}

	// Pinned by the first announce for a lobby (see GencastAnnounceIdentity.h).
	static std::mutex s_gencastAnnounceIdentityMutex;
	static GencastAnnounceIdentity s_gencastAnnounceIdentity;

	static uint64_t GencastPinnedMatchId(const std::string& lobbyId, uint64_t currentMatchId)
	{
		std::lock_guard<std::mutex> lock(s_gencastAnnounceIdentityMutex);

		bool pinnedNow = false;
		const uint64_t pinned = s_gencastAnnounceIdentity.pin(lobbyId, currentMatchId, pinnedNow);

		if (pinnedNow)
		{
			GencastDebugLog("gencast: announce identity pinned lobby=%s match_id=%llu "
				"(GO reports %llu; a later id must not move the relay's entry)",
				lobbyId.c_str(), (unsigned long long)pinned, (unsigned long long)currentMatchId);
		}
		else if (currentMatchId != 0 && currentMatchId != pinned)
		{
			GencastDebugLog("gencast: announce identity held lobby=%s pinned=%llu GO_now=%llu",
				lobbyId.c_str(), (unsigned long long)pinned, (unsigned long long)currentMatchId);
		}

		return pinned;
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
			const std::string& mapName, const std::string& lobbyName,
			int delaySeconds) override
		{
			m_playerId = playerId;
			m_userId = userId;
			m_displayName = displayName;
			m_mapName = mapName;
			m_lobbyName = lobbyName;
			m_delaySeconds = delaySeconds;
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
					// The host's title for the lobby, and the lobby's minimum watch delay: both
					// are display data the relay echoes from the announce. Absent on an older
					// relay, which is why the delay is -1 ("not said") rather than 0.
					entry.lobbyName = lobby.value("lobby_name", "");
					entry.delaySeconds = lobby.value("delay_seconds", -1);
					// The host's match-start countdown, mirrored by the relay. In direct mode this
					// is the only source: GO's observer push needs the /observers report.
					entry.countdownStarted = lobby.value("countdown_started", false);
					entry.countdownEndsAtUnixMs = lobby.value("countdown_ends_at_unix_ms", 0LL);
					entry.state = lobby.value("state", "");
					// Which lobbies cost the group's password. The watch list gates its button on
					// this, so it must come from the group and not from a blanket flag.
					entry.hasPassword = lobby.value("requires_password",
						lobby.value("has_password", false));

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

		void QueryStreamPermission(uint64_t userId, const std::string& matchId,
			GencastStreamPermissionCallback done) override
		{
			// Fail closed: everything below leaves this at "no" unless the relay explicitly says
			// otherwise. A 404 from a relay that does not have the endpoint yet, a timeout, an
			// unreadable body and a body without may_stream are all the same answer.
			GencastStreamPermission permission;

			if (!done)
			{
				return;
			}

			if (userId == 0)
			{
				// The right hangs on an account, so there is no question to ask. Asking about
				// nobody would also let the answer be applied to whoever happens to be playing.
				GencastDebugLog("gencast: stream permission skipped, no user id");
				done(permission);
				return;
			}

			std::string url = BaseUrl() + "/direct/stream-permission?user_id="
				+ std::to_string((unsigned long long)userId);

			// Which game: the same id the watch list and the announce use, so the answer is about
			// the match the player is in rather than about the lobby's newest one.
			if (!matchId.empty())
			{
				url += "&match_id=" + matchId;
			}

			HttpResponse response;
			const bool answered = Perform(url, nullptr, response);

			if (!answered || response.status != 200)
			{
				GencastDebugLog("gencast: stream permission user=%llu match=%s -> %s status=%ld, "
					"treated as denied",
					(unsigned long long)userId, matchId.c_str(),
					answered ? "refused" : "relay unreachable", response.status);
				done(permission);
				return;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);

				permission.canStream = body.value("can_stream", false);
				permission.matchEnabled = body.value("match_enabled", false);
				// may_stream is the field the client acts on, and it is never derived here: a
				// body that omits it is a refusal, not an invitation to compute can_stream ||
				// match_enabled from whatever else arrived.
				permission.mayStream = body.value("may_stream", false);
				permission.reason = body.value("reason", std::string());
			}
			catch (const nlohmann::json::exception&)
			{
				// Unreadable is not permission either.
				permission = GencastStreamPermission();
			}

			GencastDebugLog("gencast: stream permission user=%llu match=%s -> may=%d can=%d "
				"enabled=%d reason=%s",
				(unsigned long long)userId, matchId.c_str(), permission.mayStream ? 1 : 0,
				permission.canStream ? 1 : 0, permission.matchEnabled ? 1 : 0,
				permission.reason.empty() ? "(none)" : permission.reason.c_str());

			done(permission);
		}

		/// Fire-and-forget: "this account exists, and it may want to watch something".
		///
		/// Reached from the login completion on the game thread, so the body is built here and
		/// only the finished request goes to a worker - Perform() is synchronous with the same
		/// five second timeout as the rest of the transport, and a login must not wait on it.
		///
		/// The client id is GencastClientId(), the value AttemptWatchSession sends as client_id:
		/// the relay stores it on the player row and shows it in a takeover dialog, so a second
		/// identity minted here would name a connection that never joins.
		void RegisterWatcher(uint64_t userId, const std::string& displayName) override
		{
			if (userId == 0)
			{
				GencastDebugLog("gencast: watcher register skipped, no GO user id");
				return;
			}

			const std::string clientId = GencastClientId();

			nlohmann::json request;
			// The GO Services user id: the identity /direct/join takes, and the one the relay
			// keys the player row on. A record id would register an account nobody can join as.
			request["user_id"] = (unsigned long long)userId;
			request["client_id"] = clientId;

			// Refresh only: an absent name leaves whatever the relay already stored. The name
			// comes from GO and may not be valid UTF-8, which is why dump() is guarded below.
			if (!displayName.empty())
			{
				request["display_name"] = displayName;
			}

			std::string body;

			try
			{
				body = request.dump();
			}
			catch (const nlohmann::json::exception&)
			{
				GencastDebugLog("gencast: watcher register user=%llu client=%s skipped, "
					"details unencodable",
					(unsigned long long)userId, clientId.c_str());
				return;
			}

			const std::string url = BaseUrl() + "/direct/watcher/register";

			try
			{
				std::thread([url, body, userId, clientId]()
					{
						// A couple of tries, not a loop: this is a convenience for the next join,
						// never a precondition for playing, and a relay that is down must cost
						// the player one log line rather than a request every few seconds.
						const int kMaxAttempts = 3;

						int attempts = 0;
						HttpResponse response;
						bool answered = false;

						for (;;)
						{
							++attempts;
							response = HttpResponse();
							answered = Perform(url, body.c_str(), response);

							// Only a missing answer and a server-side failure are accidents worth
							// a retry. Every other status is the relay's decision: 400 is this
							// client's bug, 404 is a relay with direct watch closed and 429 is a
							// rate limit that retrying in the same second would only feed.
							if (answered && response.status != 0 && response.status < 500)
							{
								break;
							}

							if (attempts >= kMaxAttempts)
							{
								break;
							}

							// 2 s, then 4 s: long enough for a relay that is still starting,
							// short enough that the last try lands while the player is in the
							// menus the Watch Live button lives in.
							std::this_thread::sleep_for(std::chrono::seconds(2 * attempts));
						}

						if (!answered || response.status != 200)
						{
							GencastDebugLog("gencast: watcher register user=%llu client=%s -> %s "
								"status=%ld attempts=%d category=(unknown) "
								"requires_password=unknown",
								(unsigned long long)userId, clientId.c_str(),
								answered ? "refused" : "relay unreachable", response.status,
								attempts);
							return;
						}

						// What the relay says about the group this account landed in. The answer
						// carries no password and this never asks for one: the line exists so a
						// tester can see which group a watcher was registered in and whether a
						// password will be needed at the join gate.
						std::string categoryName = "(none)";
						bool requiresPassword = false;

						try
						{
							const nlohmann::json answer = nlohmann::json::parse(response.body);

							categoryName = answer.value("category_name", std::string());
							requiresPassword = answer.value("requires_password", false);
						}
						catch (const nlohmann::json::exception&)
						{
							// Registered all the same: the row is what the join gate needs, and
							// the answer's details are only for this line.
						}

						if (categoryName.empty())
						{
							categoryName = "(none)";
						}

						GencastDebugLog("gencast: watcher register user=%llu client=%s -> status=200 "
							"category=%s requires_password=%d",
							(unsigned long long)userId, clientId.c_str(), categoryName.c_str(),
							requiresPassword ? 1 : 0);
					}).detach();
			}
			catch (...)
			{
				// No worker (out of resources): the next join will 403 exactly as it does today,
				// and nothing else changes.
				GencastDebugLog("gencast: watcher register user=%llu client=%s skipped, no worker",
					(unsigned long long)userId, clientId.c_str());
			}
		}

		GencastAttemptResult AttemptWatchSession(const std::string& lobbyId,
			const std::string& password, const std::string& matchId, uint64_t watcherId,
			bool kickExisting) override
		{
			GencastAttemptResult result;

			// Every join names the account it is for. The relay rejects one without a watcher id
			// (400), and a client that has no id to send has no session to ask for either - so
			// this is refused here rather than turned into a request that cannot be honoured.
			if (watcherId == 0)
			{
				GencastDebugLog("gencast: join lobby=%s skipped, no watcher id", lobbyId.c_str());

				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "Sign in to Generals Online to watch a game.";
				return result;
			}

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["password"] = password;
			// The GO user id, which is what the relay gates the watch group, the watcher's own
			// password and the duplicate rule on.
			request["watcher_id"] = (unsigned long long)watcherId;

			// Who is asking, so a 409 can tell the next client which connection it would be
			// disconnecting. Purely descriptive: the relay never grants anything from it.
			// The same id RegisterWatcher stores on this account's player row - one identity per
			// install, in both requests, or the takeover dialog names a client that never joins.
			request["client_id"] = GencastClientId();

			// Only after the player confirmed taking the account's other connection over.
			if (kickExisting)
			{
				request["kick_existing"] = true;
			}

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

			// This account already watches this match. Not a refusal to retry and not a password
			// problem: the relay reports when and from where that other connection came, and the
			// player is the only one who can say whether it may be dropped. Returning the details
			// intact is the whole point - the dialog cannot name the connection without them.
			if (response.status == 409)
			{
				result.kind = EGencastAttempt::Duplicate;

				try
				{
					const nlohmann::json body = nlohmann::json::parse(response.body);

					// Read each field on its own: the relay may print `since` as a string or as
					// an epoch number, and one unexpected type must not throw away the other
					// field the dialog needs to name the connection.
					if (body.contains("since"))
					{
						result.duplicateSince = body["since"].is_string()
							? body["since"].get<std::string>() : body["since"].dump();
					}

					if (body.contains("client"))
					{
						result.duplicateClient = body["client"].is_string()
							? body["client"].get<std::string>() : body["client"].dump();
					}

					result.errorMessage = body.value("detail",
						std::string("This account is already watching this game."));
				}
				catch (const nlohmann::json::exception&)
				{
					// A 409 without a readable body is still a 409: the dialog falls back to
					// wording that names no particular connection.
					result.errorMessage = "This account is already watching this game.";
				}

				GencastDebugLog("gencast: join lobby=%s -> duplicate session since=%s client=%s",
					lobbyId.c_str(), result.duplicateSince.c_str(), result.duplicateClient.c_str());

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

				// A lobby that has not started yet answers with a pregame session: the lobby view
				// is fed from it, and the same session becomes the watch session when the stream
				// attaches. An older relay sends neither field, which reads as "live" - the shape it
				// has always had.
				result.pregameUrl = body.value("pregame_url", "");
				result.eventsUrl = body.value("events_url", "");
				result.live = body.value("live", true);

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

			if (result.wsUrl.empty() && result.pregameUrl.empty())
			{
				result.kind = EGencastAttempt::Failed;
				result.errorMessage = "The relay returned no stream address.";
				return result;
			}

			GencastDebugLog("gencast: join lobby=%s -> session=%s ws=%s pregame=%s live=%s",
				lobbyId.c_str(), result.sessionId.c_str(), result.wsUrl.c_str(),
				result.pregameUrl.c_str(), result.live ? "yes" : "no");

			result.kind = EGencastAttempt::Success;
			return result;
		}

		/// <summary>
		/// One request of the pregame feed: the lobby as the host announced it, every chat line after
		/// the caller's cursor, and the cursor to ask from next time.
		/// </summary>
		bool PostLobbyChat(const std::string& lobbyId, const std::string& from,
			const std::string& kind, const std::string& text) override
		{
			nlohmann::json body;
			body["lobby_id"] = lobbyId;
			body["from"] = from;
			body["kind"] = kind;
			body["text"] = text;

			HttpResponse response;

			const std::string payload = body.dump();

			return Perform(BaseUrl() + "/direct/chat", payload.c_str(), response) &&
				response.status == 200;
		}

		bool FetchPregameEvents(const std::string& eventsUrl, long long since,
			GencastPregameEvents& out) override
		{
			if (eventsUrl.empty())
			{
				return false;
			}

			char query[64];
			snprintf(query, sizeof(query), "?since=%lld", since);

			HttpResponse response;

			if (!Perform(BaseUrl() + eventsUrl + query, nullptr, response) || response.status != 200)
			{
				return false;
			}

			try
			{
				const nlohmann::json body = nlohmann::json::parse(response.body);

				if (!body.contains("snapshot"))
				{
					return false;
				}

				const nlohmann::json& snapshot = body["snapshot"];

				out.live = snapshot.value("phase", std::string("pregame")) == "live";
				out.countdownStarted = snapshot.value("countdown_started", false);
				out.countdownEndsAtUnixMs = snapshot.value("countdown_ends_at_unix_ms", 0LL);
				out.lobbyName = snapshot.value("lobby_name", std::string());
				out.delaySeconds = snapshot.value("delay_seconds", -1);
				out.next = body.value("next", 0LL);

				if (!out.live)
				{
					out.startsInSeconds = -1;
				}

				out.slots.clear();

				if (snapshot.contains("slots") && snapshot["slots"].is_array())
				{
					for (const nlohmann::json& slot : snapshot["slots"])
					{
						GencastLobbySlot parsed;
						parsed.slotIndex = slot.value("slot_index", -1);
						parsed.state = slot.value("state", std::string("open"));
						parsed.displayName = slot.value("display_name", std::string());
						parsed.ready = slot.value("ready", false);
						parsed.isAi = slot.value("is_ai", false);

						if (parsed.slotIndex >= 0)
						{
							out.slots.push_back(parsed);
						}
					}
				}

				out.chat.clear();

				if (body.contains("chat") && body["chat"].is_array())
				{
					for (const nlohmann::json& line : body["chat"])
					{
						GencastLobbyChat chat;
						chat.from = line.value("from", std::string());
						chat.text = line.value("text", std::string());
						chat.kind = line.value("kind", std::string("player"));
						chat.atUnixMs = line.value("at_unix_ms", 0LL);

						if (!chat.text.empty())
						{
							out.chat.push_back(chat);
						}
					}
				}
			}
			catch (const nlohmann::json::exception&)
			{
				return false;
			}

			return true;
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
			// one is an exact "did anything change" test: a readiness flag, a countdown that
			// started or stopped, a new seat, team, colour, map or phase all cost one request,
			// while a refresh that changes none of them costs nothing. (A body carries the lobby
			// id, so another lobby always differs.)
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

		/// One line of lobby chat, mirrored to the relay's pregame feed.
		///
		/// The host's client is the only one with a lobby's live chat, and it is the client the
		/// relay binds the entry to, so this is how a watcher reads the conversation. The body is
		/// built here, on the caller's thread (the websocket tick, which is the game thread), and
		/// only the finished request goes to a worker - same contract as the announce, and for the
		/// same reason: Perform() is synchronous with a five second timeout.
		void ForwardLobbyChat(const std::string& from, const std::string& text) override
		{
			const std::string lobbyId = CurrentLobbyId();

			if (lobbyId.empty() || text.empty())
			{
				return;
			}

			nlohmann::json request;
			request["lobby_id"] = lobbyId;
			request["match_id"] = std::to_string(GencastPinnedMatchId(lobbyId, CurrentMatchId()));
			request["from"] = from;
			// The relay has two kinds on one channel; this one only ever posts what a player
			// typed, and marks it so a watcher's own lines stay distinguishable.
			request["kind"] = "player";
			request["text"] = text;

			std::string body;

			try
			{
				body = request.dump();
			}
			catch (const nlohmann::json::exception&)
			{
				// A display name that is not valid UTF-8. Dropped rather than repaired: half a
				// name in a chat line is worse than the line not arriving.
				GencastDebugLog("gencast: chat lobby=%s dropped, details unencodable", lobbyId.c_str());
				return;
			}

			PostChat(lobbyId, body);
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
			request["match_id"] = std::to_string(GencastPinnedMatchId(lobbyId, matchId));

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

		/// POST one chat line on a worker and forget it.
		///
		/// Reached from the websocket tick, which is the game thread: the five second timeout
		/// has no place in a frame. The worker holds nothing but strings, so the transport may
		/// be deleted under it - the same contract as the announce and the lifecycle posts.
		void PostChat(const std::string& lobbyId, const std::string& body)
		{
			const std::string url = BaseUrl() + "/direct/chat";

			std::thread([url, lobbyId, body]()
				{
					HttpResponse response;
					const bool answered = Perform(url, body.c_str(), response);

					// An older relay has no chat route (404) and a missing one does not
					// answer at all. Both are the same nothing: one log line, no retry.
					if (!answered || response.status != 200)
					{
						GencastDebugLog("gencast: chat lobby=%s -> %s status=%ld",
							lobbyId.c_str(), answered ? "refused" : "relay unreachable",
							response.status);
						return;
					}

					GencastDebugLog("gencast: chat lobby=%s -> status=%ld",
						lobbyId.c_str(), response.status);
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
			const uint64_t matchId = GencastPinnedMatchId(lobbyId, CurrentMatchId());

			request["match_id"] = std::to_string(matchId);

			// The relay keys its player record on the Services user id and only falls back to
			// the wire player id, so an unknown id is left out rather than sent as -1.
			if (m_userId > 0)
			{
				request["user_id"] = m_userId;
			}

			request["display_name"] = m_displayName;
			request["map_name"] = m_mapName;
			// Display data for the watch list: what the host called the lobby and the minimum
			// watch delay it runs with. Omitted when GO does not have them, so the relay keeps
			// whatever it already knew instead of blanking the row.
			if (!m_lobbyName.empty())
			{
				request["lobby_name"] = m_lobbyName;
			}
			if (m_delaySeconds >= 0)
			{
				request["delay_seconds"] = m_delaySeconds;
			}
			// The roster now travels in slots; players is kept as an empty array because the
			// relay still carries the field, and nothing else fills it any more.
			request["players"] = nlohmann::json::array();
			request["slots"] = SlotsFromCurrentLobby();
			// The phase is the caller's, never assumed here: this body is sent both from the
			// lobby (pregame) and from the streamer at match start (live).
			request["state"] = GencastStreamStateName(state);

			int64_t mirroredEndsAtUnixMs = 0;

			const bool mirroredCountdown = state == EGencastStreamState::Pregame &&
				GencastMirrorCountdownEndsAt(mirroredEndsAtUnixMs);

			const bool countdownStarted = state == EGencastStreamState::Pregame &&
				((TheNGMPGame != nullptr && TheNGMPGame->IsCountdownStarted()) || mirroredCountdown);

			request["countdown_started"] = countdownStarted;

			if (countdownStarted && !(TheNGMPGame != nullptr && TheNGMPGame->IsCountdownStarted()))
			{
				// The mirrored one is already an absolute wall-clock end: it was computed from this
				// client's own system_clock when the host's line arrived, so it needs no skew fix.
				request["countdown_ends_at_unix_ms"] = mirroredEndsAtUnixMs;

				GencastDebugLog("gencast: announce countdown (mirrored) ends_in_ms=%lld",
					(long long)(mirroredEndsAtUnixMs - std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count()));
			}
			else if (state != EGencastStreamState::Pregame)
			{
				GencastClearMirrorCountdown();
			}

			if (countdownStarted && TheNGMPGame != nullptr && TheNGMPGame->IsCountdownStarted())
			{

				// The length the host's own on-screen timer counts down, not a guess: the same
				// GetTotalCountdownDuration() WOLGameSetupMenu subtracts from the start time.
				//
				// The start time itself is a utc_clock stamp, and utc_clock counts leap seconds
				// while the Unix epoch does not: on this toolchain it runs exactly 27 s ahead of
				// system_clock (measured, 2026-10-27). A watcher comparing it to its own wall
				// clock therefore saw a five second countdown as 32 s away, and refused to run it -
				// which is why direct mode had no countdown at all. Convert once, here, where the
				// number leaves the game, and let the field name stay true.
				static const int64_t kUtcToUnixSkewMs = []() -> int64_t
				{
					const int64_t sys = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count();
					const int64_t utc = std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::utc_clock::now().time_since_epoch()).count();

					return utc - sys;
				}();

				const int64_t endsAtUnixMs = TheNGMPGame->GetCountdownStartTime() -
					kUtcToUnixSkewMs + (int64_t)TheNGMPGame->GetTotalCountdownDuration() * 1000;

				request["countdown_ends_at_unix_ms"] = endsAtUnixMs;

				// One line per announce that carries a countdown: the host's half of the chain.
				// Without it a watcher that shows nothing cannot be told apart from a host that
				// never said anything, which is exactly how the last round was spent.
				GencastDebugLog("gencast: announce countdown ends_in_ms=%lld",
					(long long)(endsAtUnixMs - std::chrono::duration_cast<std::chrono::milliseconds>(
						std::chrono::system_clock::now().time_since_epoch()).count()));
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

		long m_playerId = 0;
		// The Services user id, or <= 0 when the client is not signed in. The relay keys its
		// player record on this one and falls back to m_playerId for older clients.
		long m_userId = 0;
		std::string m_displayName;
		std::string m_mapName;
		// Display data for the watch list: the host's title and the lobby's minimum watch delay.
		// Empty/-1 when GO does not have them; the relay then lists the map alone.
		std::string m_lobbyName;
		int m_delaySeconds = -1;
	};
}

// The two entry points other translation units call. Their work lives inside the unnamed namespace
// above, beside the mirror state they touch; these are the external names GencastTransport.h
// declares, which the chat handler and the websocket case call.
void GencastNoteLobbyAnnouncement(const std::string& message, bool isAnnouncement)
{
	GencastNoteLobbyAnnouncementInternal(message, isAnnouncement);
}

void GencastNoteHostCountdownStarted()
{
	GencastNoteHostCountdownStartedInternal();
}

GencastTransport* CreateRelayGencastTransport()
{
	return new RelayGencastTransport();
}

#endif // GENERALS_ONLINE
