#pragma once

#include <string>

namespace pbr {

/** Shorten relay ids for UI (e.g. relay:lHaEnkO4…): people's labels and messaging's notices use it. */
inline std::string ShortRelayId(const std::string& relay_or_peer_id) {
  if (relay_or_peer_id.empty()) {
    return {};
  }
  constexpr size_t kKeep = 12;
  if (relay_or_peer_id.size() <= kKeep + 1) {
    return relay_or_peer_id;
  }
  // Prefer shortening after "relay:" prefix when present.
  constexpr const char* kPrefix = "relay:";
  constexpr size_t kPrefixLen = 6;
  if (relay_or_peer_id.compare(0, kPrefixLen, kPrefix) == 0) {
    const std::string rest = relay_or_peer_id.substr(kPrefixLen);
    if (rest.size() <= 8) {
      return relay_or_peer_id;
    }
    return std::string(kPrefix) + rest.substr(0, 8) + "…";
  }
  return relay_or_peer_id.substr(0, kKeep) + "…";
}

} // namespace pbr
