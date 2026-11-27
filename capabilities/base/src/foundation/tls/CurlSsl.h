#pragma once

#include <curl/curl.h>

namespace pbr {

/** Apply platform TLS trust defaults (iOS SecTrust / Android CAPATH / desktop no-op). */
void ApplyCurlSslDefaults(CURL* curl);

/**
 * Tests only: transfers that go through ApplyCurlSslDefaults ignore the proxy variables of the environment
 * (HTTP_PROXY, ALL_PROXY, ...), so a test that connects to a closed loopback port gets "connection refused" at once
 * on a machine with a proxy configured too (a proxy that is slow or unreachable made it wait past the test's bound).
 */
void SetCurlIgnoreEnvProxyForTest(bool ignore);

/**
 * Lab and development only: true in a Debug desktop build that has `PP_BROWSER_CA_BUNDLE` set (the lab's own CA).
 * The same opt-in relaxes the foreign-relay address policy, so lab relays on private addresses stay reachable.
 */
bool DevCaBundleActive();

} // namespace pbr
