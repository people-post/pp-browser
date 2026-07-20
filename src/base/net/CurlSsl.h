#pragma once

#include <curl/curl.h>

namespace pbr {

/** Apply platform TLS defaults (e.g. Android system CAPATH) to a curl easy handle. */
void ApplyCurlSslDefaults(CURL* curl);

} // namespace pbr
