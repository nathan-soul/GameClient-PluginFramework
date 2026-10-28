#pragma once

#include <curl/curl.h>

// One place that decides how the watch sockets trust TLS: the Windows store in release,
// verification off in _DEBUG - the same split GeneralsOnline uses elsewhere.
void GencastApplyTlsTrust(CURL* easy, const char* what);
