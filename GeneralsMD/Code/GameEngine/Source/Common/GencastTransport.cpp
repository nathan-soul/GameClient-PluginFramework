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

GencastTransport* GencastTransport::Create()
{
	const EGencastMode mode = DetermineMode();

	// The two implementations land next: Services in the transport-services unit, Direct in the
	// transport-relay unit. Until then the resolved mode is logged once so it can be verified
	// against a vanilla deployment and a custom one, and no caller consumes the result yet.
	DEBUG_LOG(("[GENCAST] mode=%s", GencastModeName(mode)));
	// DEBUG_LOG is compiled out in release builds, which leaves mode unreferenced there and
	// C4189 is an error in this build.
	(void)mode;

	return nullptr;
}

#endif // GENERALS_ONLINE
