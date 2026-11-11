#include "lib/messaging/RecentPeerWarmth.h"

#include <algorithm>

namespace pbr {

bool RecentPeerWarmth::Warm(const std::string& peer_key, const int64_t now_ms) {
  if (peer_key.empty()) {
    return false;
  }
  auto [it, inserted] = last_active_ms_.try_emplace(peer_key, now_ms);
  if (!inserted) {
    it->second = std::max(it->second, now_ms);
  }
  return inserted;
}

bool RecentPeerWarmth::Touch(const std::string& peer_key, const int64_t now_ms) {
  auto it = last_active_ms_.find(peer_key);
  if (it == last_active_ms_.end()) {
    return false;
  }
  it->second = std::max(it->second, now_ms);
  return true;
}

std::vector<std::string> RecentPeerWarmth::Lapsed(const int64_t now_ms) {
  std::vector<std::string> out;
  for (auto it = last_active_ms_.begin(); it != last_active_ms_.end();) {
    if (now_ms - it->second >= keep_ms_) {
      out.push_back(it->first);
      it = last_active_ms_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

} // namespace pbr
