#pragma once

namespace pbr::os {

/**
 * Optional OpenSSL/BoringSSL CAPATH for curl (hashed PEM directory).
 * Returns nullptr when the platform provides trust another way (host CA bundle,
 * Secure Transport, Schannel) or when no system CAPATH is available.
 */
const char* TlsCaPath();

} // namespace pbr::os
