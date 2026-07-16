#include "base/messaging/RelayStreamKey.h"

#include "base/messaging/MessagingJson.h"

#include <algorithm>

namespace pbr {

std::string BuildCanonicalRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                         ThreadChannel channel, uint32_t session_epoch) {
  std::string lo = contact_id_a;
  std::string hi = contact_id_b;
  if (lo > hi) {
    std::swap(lo, hi);
  }
  return "v1:" + ThreadChannelToString(channel) + ":" + std::to_string(session_epoch) + ":" + lo + ":" + hi;
}

std::string BuildGroupRelayStreamKey(const std::string& group_id, const uint32_t session_epoch) {
  return "v1:group:" + group_id + ":" + std::to_string(session_epoch);
}

} // namespace pbr
