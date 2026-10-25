#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastClientLog.h"

#include "Common/GlobalData.h"
#include "GameClient/ClientInstance.h"
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include <curl/curl.h>

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

/*
**	Two things happen per line: an append to gencast_client_<id>.log next to the exe, and a push
**	onto a bounded queue that a detached worker drains to <relay>/direct/clientlog. The local file
**	is always written (that is the debug logging the tester can look at); the ship happens only
**	when direct mode is configured with a relay URL.
**
**	The worker is deliberately dumb: one batch per wake, drop the batch if the relay refuses it.
**	Client diagnostics are not worth unbounded buffering or retries, and a relay that is down must
**	not turn into memory growth inside the game.
*/

namespace
{
	/// Lines held while the relay is unreachable. At ~120 bytes each this is a few hundred KB.
	const size_t kMaxQueuedLines = 4000;
	/// Lines per POST: the relay caps at 2000, and a batch is one request, not one per line.
	const size_t kMaxBatchLines = 200;
	/// A single line is truncated to this; a runaway format string must not fill the queue.
	const size_t kMaxLineChars = 480;

	std::mutex s_mutex;					// guards the queue, the worker handle and the file
	std::condition_variable s_wake;
	std::deque<std::string> s_queue;
	std::thread s_worker;
	bool s_workerStarted = false;
	FILE* s_file = nullptr;

	/// Separate from s_mutex: GencastClientId() is called with s_mutex held (the file name).
	std::mutex s_idMutex;
	std::string s_clientId;
	bool s_idResolved = false;

	/// Trailing-slash-free relay base, or empty when direct mode is not configured.
	///
	/// Per-thread scratch: this is called from the game thread and from the ship worker, and a
	/// shared mutable static here would be a data race on a std::string.
	const std::string& RelayBase()
	{
		static thread_local std::string base;

		base = NGMP_OnlineServicesManager::Settings.Relay_Direct_Url();

		while (!base.empty() && base[base.size() - 1] == '/')
		{
			base.erase(base.size() - 1);
		}

		return base;
	}

	/// Shipping is a direct-mode feature: without a relay there is nowhere to send it.
	bool ShippingConfigured()
	{
		return NGMP_OnlineServicesManager::Settings.Relay_Direct() && !RelayBase().empty();
	}

	std::string SanitizeId(const std::string& raw)
	{
		std::string out;

		for (size_t i = 0; i < raw.size() && out.size() < 48; ++i)
		{
			const char c = raw[i];

			if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')
			{
				out.push_back(c);
			}
			else if (c >= 'A' && c <= 'Z')
			{
				out.push_back((char)(c - 'A' + 'a'));
			}
			else if (c == ' ' || c == '_' || c == '.')
			{
				out.push_back('-');
			}
		}

		return out.empty() ? std::string("client") : out;
	}

	std::string MakeClientId()
	{
		char host[256] = {};
		DWORD hostLen = sizeof(host) - 1;

		if (!::GetComputerNameA(host, &hostLen))
		{
			snprintf(host, sizeof(host), "win");
		}

		std::random_device random;
		char suffix[16];
		snprintf(suffix, sizeof(suffix), "-%03x%03x", random() & 0xFFF, random() & 0xFFF);

		return SanitizeId(host) + suffix;
	}

	/// Reads the saved id, or mints and saves one. Written next to settings.json so it survives
	/// reinstalls of the exe but stays per machine+user.
	std::string LoadOrCreateClientId()
	{
		std::string dir = "GeneralsOnlineData/";

		if (TheGlobalData != nullptr)
		{
			dir = std::string(TheGlobalData->getPath_UserData().str()) + "/GeneralsOnlineData/";
		}

		const std::string path = dir + "gencast_client_id.txt";

		{
			std::ifstream existing(path);

			if (existing.good())
			{
				std::string line;
				std::getline(existing, line);

				while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == '\n'))
				{
					line.erase(line.size() - 1);
				}

				if (!line.empty())
				{
					return SanitizeId(line);
				}
			}
		}

		const std::string created = MakeClientId();

		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		std::ofstream out(path, std::ios::trunc);
		if (out.good())
		{
			out << created << "\n";
		}

		return created;
	}

	void EnsureFileLocked()
	{
		if (s_file != nullptr)
		{
			return;
		}

		char name[128];
		snprintf(name, sizeof(name), "gencast_client_%s.log", GencastClientId());
		s_file = fopen(name, "a");

		if (s_file != nullptr)
		{
			SYSTEMTIME st;
			GetLocalTime(&st);
			fprintf(s_file, "=== gencast client log opened %04u-%02u-%02u %02u:%02u:%02u (client=%s instance=%d) ===\n",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
				GencastClientId(), GencastClientInstance());
			fflush(s_file);
		}
	}

	size_t DiscardResponse(char* /*ptr*/, size_t size, size_t nmemb, void* /*userdata*/)
	{
		return size * nmemb;
	}

	bool PostBatch(const std::string& payload)
	{
		const std::string url = RelayBase() + "/direct/clientlog";

		CURL* curl = curl_easy_init();
		if (curl == nullptr)
		{
			return false;
		}

		curl_slist* headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
		// Short: this runs behind a live match and a stuck relay must not pin the worker.
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, DiscardResponse);

		const CURLcode rc = curl_easy_perform(curl);

		long status = 0;
		if (rc == CURLE_OK)
		{
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		}

		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);

		return rc == CURLE_OK && status == 200;
	}

	void ShipBatch(const std::vector<std::string>& batch)
	{
		try
		{
			nlohmann::json request;
			request["client_id"] = GencastClientId();
			request["instance"] = GencastClientInstance();
			request["lines"] = batch;

			PostBatch(request.dump());
		}
		catch (const std::exception&)
		{
			// A line that cannot be serialised loses the batch, never the game.
		}
	}

	void Worker()
	{
		for (;;)
		{
			std::vector<std::string> batch;

			{
				std::unique_lock<std::mutex> lock(s_mutex);

				s_wake.wait_for(lock, std::chrono::seconds(1),
					[] { return !s_queue.empty(); });

				while (!s_queue.empty() && batch.size() < kMaxBatchLines)
				{
					batch.push_back(std::move(s_queue.front()));
					s_queue.pop_front();
				}
			}

			if (batch.empty())
			{
				continue;
			}

			if (ShippingConfigured())
			{
				ShipBatch(batch);
			}
		}
	}

	void EnsureWorkerLocked()
	{
		if (s_workerStarted)
		{
			return;
		}

		s_workerStarted = true;

		// Detached: the game exits on its own schedule and a test build must never hang on a
		// half-finished POST. The queue is best-effort by design.
		s_worker = std::thread(Worker);
		s_worker.detach();
	}
}

const char* GencastClientId()
{
	std::lock_guard<std::mutex> lock(s_idMutex);

	if (!s_idResolved)
	{
		s_clientId = LoadOrCreateClientId();
		s_idResolved = true;
	}

	return s_clientId.c_str();
}

int GencastClientInstance()
{
	return (int)rts::ClientInstance::getInstanceId();
}

void GencastClientLogLine(const char* text)
{
	if (text == nullptr || *text == '\0')
	{
		return;
	}

	std::string line(text);

	while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
	{
		line.erase(line.size() - 1);
	}

	if (line.empty())
	{
		return;
	}

	if (line.size() > kMaxLineChars)
	{
		line.resize(kMaxLineChars);
	}

	SYSTEMTIME st;
	GetLocalTime(&st);

	char stamp[64];
	snprintf(stamp, sizeof(stamp), "[%02u:%02u:%02u.%03u] ",
		st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

	const std::string stamped = std::string(stamp) + line;

	std::lock_guard<std::mutex> lock(s_mutex);

	EnsureFileLocked();

	if (s_file != nullptr)
	{
		fputs(stamped.c_str(), s_file);
		fputc('\n', s_file);
		fflush(s_file);
	}

	if (!ShippingConfigured())
	{
		return;
	}

	if (s_queue.size() >= kMaxQueuedLines)
	{
		s_queue.pop_front();
	}

	s_queue.push_back(stamped);

	EnsureWorkerLocked();
	s_wake.notify_one();
}

void GencastDebugLog(const char* fmt, ...)
{
	if (fmt == nullptr)
	{
		return;
	}

	char buffer[768] = {};

	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer) - 1, fmt, args);
	va_end(args);

	GencastClientLogLine(buffer);
}

#endif // GENERALS_ONLINE
