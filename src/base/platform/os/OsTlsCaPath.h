#pragma once

namespace pbr::os {

/**
 * Optional OpenSSL/BoringSSL CAPATH for curl (hashed PEM directory).
 * Used by Android via ApplyPlatformCurlSsl. Returns nullptr when the platform
 * provides trust another way (iOS SecTrust, Secure Transport, Schannel, host
 * CA bundle) or when no system CAPATH is available.
 */
const char* TlsCaPath();

} // namespace pbr::os
