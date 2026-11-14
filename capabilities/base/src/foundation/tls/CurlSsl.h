#pragma once

#include <curl/curl.h>

namespace pbr {

/** Apply platform TLS trust defaults (iOS SecTrust / Android CAPATH / desktop no-op). */
void ApplyCurlSslDefaults(CURL* curl);

/**
 * Lab and development only: true in a Debug desktop build that has `PP_BROWSER_CA_BUNDLE` set (the lab's own CA).
 * The same opt-in relaxes the foreign-relay address policy, so lab relays on private addresses stay reachable.
 */
bool DevCaBundleActive();

} // namespace pbr
