#include "lib/delivery/RelayOrigins.h"

#include "lib/people/RegistrationStatus.h"

#include "common/MeshAlias.h"
#include "mesh/types/PublicAddress.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace pbr {

namespace {

bool IsHttpUrl(const std::string& url) {
  const std::string_view view(url);
  for (const std::string_view scheme : {std::string_view("https://"), std::string_view("http://")}) {
    if (view.size() > scheme.size() && view.substr(0, scheme.size()) == scheme) {
      const std::string_view rest = view.substr(scheme.size());
      return !rest.empty() && rest.front() != '/' &&
             std::none_of(rest.begin(), rest.end(), [](char c) { return c == ' ' || c == '?' || c == '#'; });
    }
  }
  return false;
}

bool EqualsIgnoreCase(std::string_view text, std::string_view lower) {
  return text.size() == lower.size() && std::equal(text.begin(), text.end(), lower.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) == b;
         });
}

bool HasSuffix(std::string_view host, std::string_view suffix) {
  return host.size() > suffix.size() && host.ends_with(suffix);
}

/** The last label of a DNS name is never numeric (`0x7f.1`, `127.1` and the like are address spellings). */
bool NumericLastLabel(std::string_view host) {
  const std::string_view label = host.substr(host.rfind('.') == std::string_view::npos ? 0 : host.rfind('.') + 1);
  return !label.empty() && (std::all_of(label.begin(), label.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                            (label.size() >= 2 && label[0] == '0' && (label[1] == 'x' || label[1] == 'X')));
}

/** A dotted quad in canonical form only: inet_pton differs on leading zeros (`0177.0.0.1`: refused by glibc, decimal on macOS) while the resolver may read them as octal. */
bool IsCanonicalDottedQuad(std::string_view host) {
  int parts = 0;
  size_t start = 0;
  while (start <= host.size()) {
    size_t end = host.find('.', start);
    if (end == std::string_view::npos) {
      end = host.size();
    }
    const std::string_view part = host.substr(start, end - start);
    if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') || std::stoi(std::string(part)) > 255) {
      return false;
    }
    ++parts;
    start = end + 1;
  }
  return parts == 4;
}

} // namespace

bool IsForeignRelayOriginAllowed(const std::string& origin, const bool lab_relaxed) {
  const std::string_view view(origin);
  std::string_view rest;
  if (view.size() > 8 && EqualsIgnoreCase(view.substr(0, 8), "https://")) {
    rest = view.substr(8);
  } else if (lab_relaxed && view.size() > 7 && EqualsIgnoreCase(view.substr(0, 7), "http://")) {
    rest = view.substr(7);
  } else {
    return false;
  }
  const std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  if (authority.empty() || authority.find('@') != std::string_view::npos ||
      std::any_of(authority.begin(), authority.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte <= 0x20 || byte == 0x7f || c == '\\';
      })) {
    return false;
  }
  std::string_view host = authority;
  const bool bracketed = authority.front() == '[';
  if (bracketed) {
    const size_t close = authority.find(']');
    if (close == std::string_view::npos || close < 2) {
      return false;
    }
    host = authority.substr(1, close - 1);
  } else if (const size_t colon = authority.find(':'); colon != std::string_view::npos) {
    host = authority.substr(0, colon);
  }
  if (lab_relaxed) {
    return !host.empty();
  }
  if (bracketed) {
    uint8_t addr[16];
    return inet_pton(AF_INET6, std::string(host).c_str(), addr) == 1 && mesh::IsPublicIPv6(addr);
  }
  while (host.ends_with('.')) {
    host.remove_suffix(1);
  }
  if (host.empty() || host.find('.') == std::string_view::npos || host.find(':') != std::string_view::npos) {
    return false; // empty, a single-label name, or a stray colon
  }
  std::string lowered(host);
  for (char& c : lowered) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  const bool digits_and_dots =
      std::all_of(lowered.begin(), lowered.end(), [](char c) { return c == '.' || (c >= '0' && c <= '9'); });
  if (digits_and_dots && !IsCanonicalDottedQuad(lowered)) {
    return false;
  }
  struct in_addr v4{};
  if (inet_pton(AF_INET, lowered.c_str(), &v4) == 1) {
    return mesh::IsPublicIPv4(ntohl(v4.s_addr));
  }
  return !NumericLastLabel(lowered) && lowered != "localhost" && !HasSuffix(lowered, ".localhost") &&
         !HasSuffix(lowered, ".local") && !HasSuffix(lowered, ".internal");
}

std::string RelayOriginKey(const std::string& origin) {
  std::string key = NormalizeRegistrationBackend(origin); // trimmed, no trailing `/`, scheme and host lower case
  const size_t scheme_end = key.find("://");
  if (scheme_end == std::string::npos) {
    return key;
  }
  const size_t host_end = std::min(key.find('/', scheme_end + 3), key.size());
  const std::string_view scheme = std::string_view(key).substr(0, scheme_end);
  const std::string_view default_port = scheme == "https" ? ":443" : scheme == "http" ? ":80" : "";
  const std::string_view authority = std::string_view(key).substr(0, host_end);
  if (!default_port.empty() && authority.ends_with(default_port)) {
    key.erase(host_end - default_port.size(), default_port.size());
  }
  return key;
}

bool SameRelay(const std::string& a, const std::string& b) {
  return RelayOriginKey(a) == RelayOriginKey(b);
}

std::vector<std::string> OurRelayOrigins(const std::string& preferred, const std::vector<std::string>& extras) {
  std::vector<std::string> out;
  const std::string first = NormalizeRegistrationBackend(preferred);
  if (first.empty()) {
    return out;
  }
  out.push_back(first);
  for (const std::string& extra : extras) {
    if (out.size() >= kMaxOurRelays) {
      break;
    }
    const std::string origin = NormalizeRegistrationBackend(extra);
    const bool listed =
        std::any_of(out.begin(), out.end(), [&origin](const std::string& known) { return SameRelay(known, origin); });
    if (IsHttpUrl(origin) && !listed) {
      out.push_back(origin);
    }
  }
  return out;
}

} // namespace pbr
