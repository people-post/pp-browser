#include "domain/net/PublicOnlySocket.h"

#include "common/net/PublicAddress.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>

namespace pbr {

curl_socket_t OpenPublicOnlySocket(void* /*clientp*/, curlsocktype purpose, struct curl_sockaddr* address) {
  if (purpose != CURLSOCKTYPE_IPCXN || address == nullptr) {
    return CURL_SOCKET_BAD;
  }
  bool allowed = false;
  if (address->family == AF_INET) {
    const auto* sin = reinterpret_cast<const sockaddr_in*>(&address->addr);
    allowed = IsPublicIPv4(ntohl(sin->sin_addr.s_addr));
  } else if (address->family == AF_INET6) {
    const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(&address->addr);
    allowed = IsPublicIPv6(reinterpret_cast<const uint8_t*>(&sin6->sin6_addr));
  }
  if (!allowed) {
    return CURL_SOCKET_BAD;
  }
  const curl_socket_t sock = socket(address->family, address->socktype, address->protocol);
  return sock < 0 ? CURL_SOCKET_BAD : sock;
}

} // namespace pbr
