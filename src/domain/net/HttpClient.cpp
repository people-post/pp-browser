#include "domain/net/HttpClient.h"

#include "foundation/error/AppError.h"
#include "foundation/platform/CurlSsl.h"

#include "common/PbrCompat.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <curl/curl.h>

#include <cstring>
#include <limits>
#include <optional>

namespace pbr {

namespace {

struct ResponseBuffer {
  std::string body;
  std::optional<size_t> max_bytes;
  bool limit_exceeded = false;
};

size_t WriteCallback(void* contents, size_t size, size_t nmemb, ResponseBuffer* out) {
  if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
    out->limit_exceeded = true;
    return 0;
  }
  const size_t total = size * nmemb;
  if (out->max_bytes &&
      (total > *out->max_bytes || out->body.size() > *out->max_bytes - total)) {
    out->limit_exceeded = true;
    return 0;
  }
  out->body.append(static_cast<const char*>(contents), total);
  return total;
}

/** RFC 1918 / loopback / link-local / CGNAT / multicast / reserved — not a public Internet host. */
bool IsPublicIPv4(const uint32_t host_order_addr) {
  const uint32_t a = host_order_addr;
  if ((a & 0xFF000000u) == 0x00000000u) return false;              // 0.0.0.0/8
  if ((a & 0xFF000000u) == 0x0A000000u) return false;              // 10.0.0.0/8
  if ((a & 0xFF000000u) == 0x7F000000u) return false;              // 127.0.0.0/8 loopback
  if ((a & 0xFFC00000u) == 0x64400000u) return false;              // 100.64.0.0/10 CGNAT
  if ((a & 0xFFFF0000u) == 0xA9FE0000u) return false;              // 169.254.0.0/16 link-local
  if ((a & 0xFFF00000u) == 0xAC100000u) return false;              // 172.16.0.0/12
  if ((a & 0xFFFF0000u) == 0xC0A80000u) return false;              // 192.168.0.0/16
  if ((a & 0xFFFFFF00u) == 0xC0000000u) return false;              // 192.0.0.0/24 IETF
  if ((a & 0xFFFFFF00u) == 0xC0000200u) return false;              // 192.0.2.0/24 TEST-NET-1
  if ((a & 0xFFFE0000u) == 0xC6120000u) return false;              // 198.18.0.0/15 benchmark
  if ((a & 0xFFFFFF00u) == 0xC6336400u) return false;              // 198.51.100.0/24 TEST-NET-2
  if ((a & 0xFFFFFF00u) == 0xCB007100u) return false;              // 203.0.113.0/24 TEST-NET-3
  if ((a & 0xF0000000u) == 0xE0000000u) return false;              // 224.0.0.0/4 multicast
  if ((a & 0xF0000000u) == 0xF0000000u) return false;              // 240.0.0.0/4 reserved + broadcast
  return true;
}

bool AllZero(const uint8_t* bytes, const size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (bytes[i] != 0) {
      return false;
    }
  }
  return true;
}

uint32_t EmbeddedIPv4(const uint8_t* addr) {
  uint32_t v4 = 0;
  std::memcpy(&v4, addr, 4);
  return ntohl(v4);
}

/**
 * ::, ::1, fe80::/10, fec0::/10 (deprecated site-local), fc00::/7 (ULA), ff00::/8 multicast, and
 * every IPv6 form that embeds an IPv4 address (::a.b.c.d, ::ffff:a.b.c.d, 64:ff9b::/96 NAT64,
 * 2002::/16 6to4) classified by the embedded address.
 */
bool IsPublicIPv6(const in6_addr& addr) {
  const uint8_t* b = addr.s6_addr;
  if (IN6_IS_ADDR_LOOPBACK(&addr)) {
    return false;
  }
  if (AllZero(b, 16)) {
    return false; // :: unspecified
  }
  if (b[0] == 0x20 && b[1] == 0x02) {
    return IsPublicIPv4(EmbeddedIPv4(b + 2)); // 2002::/16 6to4
  }
  if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xFF && b[3] == 0x9B && AllZero(b + 4, 8)) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // 64:ff9b::/96 NAT64
  }
  if (AllZero(b, 10) && b[10] == 0xFF && b[11] == 0xFF) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // ::ffff:a.b.c.d IPv4-mapped
  }
  if (AllZero(b, 12)) {
    return IsPublicIPv4(EmbeddedIPv4(b + 12)); // ::a.b.c.d IPv4-compatible (deprecated)
  }
  if ((b[0] & 0xFE) == 0xFC) return false;               // fc00::/7 unique local
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false; // fe80::/10 link-local
  if (b[0] == 0xFE && (b[1] & 0xC0) == 0xC0) return false; // fec0::/10 site-local (deprecated)
  if (b[0] == 0xFF) return false;                          // ff00::/8 multicast
  return true;
}

/**
 * Post-DNS-resolution SSRF guard for URLs sourced from a remote peer (attachment/profile-icon
 * fetch): checking the hostname string is not enough (DNS rebinding), so this replaces curl's own
 * socket() and rejects any resolved address that is not a public-routable host.
 */
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
    allowed = IsPublicIPv6(sin6->sin6_addr);
  }
  if (!allowed) {
    return CURL_SOCKET_BAD;
  }
#if defined(_WIN32)
  const curl_socket_t sock = socket(address->family, address->socktype, address->protocol);
  return sock == INVALID_SOCKET ? CURL_SOCKET_BAD : sock;
#else
  const curl_socket_t sock = socket(address->family, address->socktype, address->protocol);
  return sock < 0 ? CURL_SOCKET_BAD : sock;
#endif
}

Roe<HttpResponse> Perform(const std::string& url, const char* method, const std::string& body,
                          const std::map<std::string, std::string>& headers,
                          std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                          bool restrict_to_public_https) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("Failed to init curl");
  }

  ApplyCurlSslDefaults(curl);

  if (restrict_to_public_https) {
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, OpenPublicOnlySocket);
    // A proxy from the environment would connect to the proxy's address, not the target's, so
    // OpenPublicOnlySocket would be checking the wrong host and a proxy could tunnel to an
    // internal address anyway.
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
  }

  ResponseBuffer response_body{.max_bytes = max_response_bytes};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  const long total_s = timeout.total_s > 0 ? timeout.total_s : 30L;
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, total_s);
  if (timeout.connect_s > 0) {
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout.connect_s);
  }

  struct curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    header_list = curl_slist_append(header_list, (key + ": " + value).c_str());
  }
  if (header_list) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  }

  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }

  const CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

  if (header_list) {
    curl_slist_free_all(header_list);
  }
  curl_easy_cleanup(curl);

  if (response_body.limit_exceeded) {
    if (!max_response_bytes) {
      return AppError::Network(Err::Network::HttpError, "HTTP response body size overflow");
    }
    return AppError::Network(Err::Network::HttpError,
                             "HTTP response body exceeds configured limit of " +
                                 std::to_string(*max_response_bytes) + " bytes");
  }

  if (code != CURLE_OK) {
    return AppError::Network(Err::Network::Unreachable,
                          std::string("HTTP request failed: ") + curl_easy_strerror(code));
  }

  return HttpResponse{.status_code = status_code, .body = std::move(response_body.body)};
}

} // namespace

Roe<HttpResponse> HttpClient::Get(const std::string& url, const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https) {
  return Perform(url, "GET", {}, headers, max_response_bytes, timeout, restrict_to_public_https);
}

Roe<HttpResponse> HttpClient::Post(const std::string& url, const std::string& body,
                                   const std::map<std::string, std::string>& headers,
                                   std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                   bool restrict_to_public_https) {
  return Perform(url, "POST", body, headers, max_response_bytes, timeout, restrict_to_public_https);
}

Roe<HttpResponse> HttpClient::Put(const std::string& url, const std::string& body,
                                  const std::map<std::string, std::string>& headers,
                                  std::optional<size_t> max_response_bytes, HttpTimeout timeout,
                                  bool restrict_to_public_https) {
  return Perform(url, "PUT", body, headers, max_response_bytes, timeout, restrict_to_public_https);
}

} // namespace pbr
