#include "lib/net/RelayOrigins.h"

#include "lib/people/RegistrationStatus.h"

#include "common/MeshAlias.h"
#include "mesh/types/PublicAddress.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

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

/**
 * `host` as a dotted quad in canonical form only (four parts of 0..255, no leading zeros), in host order; else none.
 * The OS parsers differ on leading zeros (`0177.0.0.1`: refused by glibc, decimal on macOS) while the resolver may
 * read them as octal, so no other spelling is an address here.
 */
std::optional<uint32_t> CanonicalDottedQuad(std::string_view host) {
  uint32_t address = 0;
  int parts = 0;
  size_t start = 0;
  while (start <= host.size()) {
    size_t end = host.find('.', start);
    if (end == std::string_view::npos) {
      end = host.size();
    }
    const std::string_view part = host.substr(start, end - start);
    if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') ||
        !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return std::nullopt;
    }
    const int value = std::stoi(std::string(part));
    if (value > 255 || ++parts > 4) {
      return std::nullopt;
    }
    address = (address << 8) | static_cast<uint32_t>(value);
    start = end + 1;
  }
  return parts == 4 ? std::optional<uint32_t>(address) : std::nullopt;
}

/**
 * `text` (inside the brackets) as an IPv6 address in network order: hex groups, one `::`, a canonical dotted quad as
 * the last 32 bits; no zone. Else none.
 */
std::optional<std::array<uint8_t, 16>> Ipv6Literal(std::string_view text) {
  std::array<uint16_t, 8> groups{};
  size_t count = 0;
  std::optional<size_t> gap;
  if (text.starts_with("::")) {
    gap = 0;
    text.remove_prefix(2);
  }
  while (!text.empty()) {
    const size_t colon = text.find(':');
    const std::string_view group = text.substr(0, colon);
    if (group.find('.') != std::string_view::npos) {
      const auto v4 = colon == std::string_view::npos ? CanonicalDottedQuad(group) : std::nullopt;
      if (!v4 || count > 6) {
        return std::nullopt;
      }
      groups[count++] = static_cast<uint16_t>(*v4 >> 16);
      groups[count++] = static_cast<uint16_t>(*v4 & 0xffff);
      break;
    }
    if (group.empty() || group.size() > 4 || count == groups.size() ||
        !std::all_of(group.begin(), group.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
      return std::nullopt;
    }
    groups[count++] = static_cast<uint16_t>(std::stoul(std::string(group), nullptr, 16));
    if (colon == std::string_view::npos) {
      break;
    }
    text.remove_prefix(colon + 1);
    if (text.starts_with(":")) {
      if (gap) {
        return std::nullopt;
      }
      gap = count;
      text.remove_prefix(1);
    } else if (text.empty()) {
      return std::nullopt; // a trailing lone `:`
    }
  }
  if (gap ? count >= groups.size() : count != groups.size()) {
    return std::nullopt;
  }
  std::array<uint16_t, 8> full{};
  const size_t tail = gap ? count - *gap : 0;
  for (size_t i = 0; i < count; ++i) {
    const bool after_gap = gap && i >= *gap;
    full[after_gap ? groups.size() - tail + (i - *gap) : i] = groups[i];
  }
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < full.size(); ++i) {
    bytes[2 * i] = static_cast<uint8_t>(full[i] >> 8);
    bytes[2 * i + 1] = static_cast<uint8_t>(full[i] & 0xff);
  }
  return bytes;
}

} // namespace

bool IsPublicRelayOrigin(const std::string& origin, const bool lab_relaxed) {
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
    const auto v6 = Ipv6Literal(host);
    return v6 && mesh::IsPublicIPv6(v6->data());
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
  if (digits_and_dots) {
    const auto v4 = CanonicalDottedQuad(lowered);
    return v4 && mesh::IsPublicIPv4(*v4);
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

std::string RelayOriginHost(const std::string& origin) {
  const std::string key = RelayOriginKey(origin);
  const size_t scheme_end = key.find("://");
  if (scheme_end == std::string::npos) {
    return {};
  }
  const size_t host_start = scheme_end + 3;
  std::string host = key.substr(host_start, std::min(key.find('/', host_start), key.size()) - host_start);
  if (const size_t at = host.rfind('@'); at != std::string::npos) {
    host.erase(0, at + 1); // credentials are no part of the name
  }
  return host;
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
    // A relay link names its PeerId after `#` (MR008): the origin is what comes before.
    const std::string origin = NormalizeRegistrationBackend(extra.substr(0, extra.find('#')));
    const bool listed =
        std::any_of(out.begin(), out.end(), [&origin](const std::string& known) { return SameRelay(known, origin); });
    if (IsHttpUrl(origin) && !listed) {
      out.push_back(origin);
    }
  }
  return out;
}

std::vector<std::string> BoundOurRelays(const std::vector<std::string>& list) {
  if (list.empty()) {
    return {};
  }
  return OurRelayOrigins(list.front(), std::vector<std::string>(list.begin() + 1, list.end()));
}

namespace {

constexpr std::string_view kRelayPath = "/api/relay";

/** `host[:port]`: letters, digits, `.`, `-`, `[`/`]`/`:` (an IPv6 literal), a port of digits. */
bool IsAuthority(const std::string_view authority) {
  if (authority.empty() || authority.front() == '.' || authority.front() == ':') {
    return false;
  }
  return std::all_of(authority.begin(), authority.end(), [](const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
  });
}

} // namespace

ParsedRelayOrigin ParseRelayOrigin(const std::string& text, const bool allow_http) {
  std::string url = NormalizeRegistrationBackend(text); // trimmed, no trailing `/`, scheme and host lower case
  if (url.empty()) {
    return {.problem = RelayOriginProblem::Empty};
  }
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return {.problem = RelayOriginProblem::NotAUrl};
  }
  const std::string_view scheme = std::string_view(url).substr(0, scheme_end);
  if (scheme != "https" && !(allow_http && scheme == "http")) {
    return {.problem = scheme == "http" ? RelayOriginProblem::NotHttps : RelayOriginProblem::NotAUrl};
  }
  const std::string_view rest = std::string_view(url).substr(scheme_end + 3);
  if (rest.find_first_of("?#") != std::string_view::npos) {
    return {.problem = RelayOriginProblem::HasQuery};
  }
  const size_t path_start = std::min(rest.find('/'), rest.size());
  const std::string_view authority = rest.substr(0, path_start);
  const std::string_view path = rest.substr(path_start);
  if (!IsAuthority(authority)) {
    return {.problem = RelayOriginProblem::NotAUrl};
  }
  if (!path.empty() && path != kRelayPath) {
    return {.problem = RelayOriginProblem::HasPath};
  }
  std::string base_url = url.substr(0, scheme_end + 3 + authority.size());
  base_url += kRelayPath;
  if (base_url.size() > kMaxRelayOriginBytes) {
    return {.problem = RelayOriginProblem::TooLong};
  }
  return {.base_url = std::move(base_url)};
}

ParsedRelayOrigin ParseRelayLink(const std::string& text, const bool allow_http) {
  std::string url = text;
  std::string fragment;
  if (const size_t hash = url.find('#'); hash != std::string::npos) {
    fragment = url.substr(hash + 1);
    url.resize(hash);
    while (!fragment.empty() && std::isspace(static_cast<unsigned char>(fragment.back())) != 0) {
      fragment.pop_back();
    }
  }
  ParsedRelayOrigin parsed = ParseRelayOrigin(url, allow_http);
  if (parsed.problem != RelayOriginProblem::None) {
    return parsed;
  }
  if (text.find('#') != std::string::npos) {
    const auto relay_id = RelayId::Parse(fragment);
    if (!relay_id) {
      return {.problem = RelayOriginProblem::BadRelayId};
    }
    parsed.relay_id = *relay_id;
  }
  return parsed;
}

std::string RelayLink(const std::string& base_url, const RelayId& relay_id) {
  return relay_id.empty() ? base_url : base_url + "#" + relay_id.ToString();
}

} // namespace pbr
