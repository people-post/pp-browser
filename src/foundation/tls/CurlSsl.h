#pragma once

#include <curl/curl.h>

namespace pbr {

/** Apply platform TLS trust defaults (iOS SecTrust / Android CAPATH / desktop no-op). */
void ApplyCurlSslDefaults(CURL* curl);

}  // namespace pbr
