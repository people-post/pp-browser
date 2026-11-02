#pragma once

#include "mesh/types/PublicAddress.h"
#include "common/MeshAlias.h"

#include <curl/curl.h>

#include <cstdint>

namespace pbr {

/**
 * Post-DNS-resolution SSRF guard for URLs sourced from a remote peer (attachment / profile-icon
 * fetch): checking the hostname string is not enough (DNS rebinding), so this replaces curl's own
 * socket() (CURLOPT_OPENSOCKETFUNCTION) and refuses any resolved address that is not a
 * public-routable host (`PublicAddress.h`). Per-OS backends: PublicOnlySocket_{Posix,Win32}.cpp.
 */
curl_socket_t OpenPublicOnlySocket(void* clientp, curlsocktype purpose, struct curl_sockaddr* address);

/**
 * `clientp` of OpenPublicOnlySocket (CURLOPT_OPENSOCKETDATA), set only under HttpClient::SetFirstHopLocalForTest:
 * the first `opens_left` connections skip the address check. Null (the app): every connection is checked.
 */
struct PublicOnlySocketTestExemption {
  int opens_left = 0;
};

/**
 * 198.18.0.0/15 is the "fake-IP" range that local TUN proxies (Clash, Surge, Shadowrocket, sing-box) hand out
 * for every DNS answer: the socket goes into the tunnel and the proxy resolves and connects to the real host.
 * Refusing it (the range is reserved, so `IsPublicIPv4` says no) would block every download for a user whose
 * VPN is on — and PP never bypasses the user's VPN. The address check cannot see the real host in that mode.
 */
inline bool IsLocalProxyFakeIPv4(const uint32_t host_order_addr) {
  return (host_order_addr & 0xFFFE0000u) == 0xC6120000u;
}

/** The IPv4 rule of OpenPublicOnlySocket: a public host, or a local proxy's fake IP. */
inline bool PublicOnlySocketAllowsIPv4(const uint32_t host_order_addr) {
  return mesh::IsPublicIPv4(host_order_addr) || IsLocalProxyFakeIPv4(host_order_addr);
}

} // namespace pbr
