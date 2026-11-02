#pragma once

// Match libcurl's `typedef void CURL` so this header stays free of curl includes
// (desktop platform TUs must not require linking libcurl).
typedef void CURL;

namespace pbr::os {

/**
 * Apply platform-native TLS trust to a curl easy handle.
 *
 * - iOS: BoringSSL has no system store; install an SSL_CTX verify callback that
 *   evaluates the peer chain with Security.framework (SecTrust).
 * - Android: set CURLOPT_CAPATH to the system hashed-PEM CA directory.
 * - Desktop: no-op (Secure Transport / Schannel / host CA bundle).
 */
void ApplyPlatformCurlSsl(CURL* curl);

} // namespace pbr::os
