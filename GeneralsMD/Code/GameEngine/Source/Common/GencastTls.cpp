#include "Common/GencastTls.h"
#include "Common/GencastClientLog.h"

void GencastApplyTlsTrust(CURL* easy, const char* what)
{
	if (easy == nullptr)
		return;

#if defined(_DEBUG)
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
	GencastDebugLog("gencast: tls trust=off socket=%s (debug)", what);
#else
	curl_easy_setopt(easy, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
	GencastDebugLog("gencast: tls trust=windows-store socket=%s", what);
#endif
}
