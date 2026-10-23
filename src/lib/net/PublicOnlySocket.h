#pragma once

#include <curl/curl.h>

namespace pbr {

/**
 * Post-DNS-resolution SSRF guard for URLs sourced from a remote peer (attachment / profile-icon
 * fetch): checking the hostname string is not enough (DNS rebinding), so this replaces curl's own
 * socket() (CURLOPT_OPENSOCKETFUNCTION) and refuses any resolved address that is not a
 * public-routable host (`PublicAddress.h`). Per-OS backends: PublicOnlySocket_{Posix,Win32}.cpp.
 */
curl_socket_t OpenPublicOnlySocket(void* clientp, curlsocktype purpose, struct curl_sockaddr* address);

} // namespace pbr
