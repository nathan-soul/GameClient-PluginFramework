#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/GencastTransport.h"
#include "Common/LiveObserver.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"

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
	const std::string& password, GencastSessionCallback done,
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

		const GencastAttemptResult attempt = AttemptWatchSession(lobbyId, password);

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

#endif // GENERALS_ONLINE
