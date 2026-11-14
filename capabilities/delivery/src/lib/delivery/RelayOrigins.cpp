#include "lib/delivery/RelayOrigins.h"

#include "lib/people/RegistrationStatus.h"

#include <algorithm>
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

} // namespace

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
