#include "lib/delivery/RelayHealth.h"

#include "lib/delivery/RelayOrigins.h"

#include <algorithm>

namespace pbr {

bool RelayHealth::FailedLocked(const std::string& key, const int64_t now_ms) const {
  const auto it = failed_at_.find(key);
  return it != failed_at_.end() && now_ms - it->second < kRelayFailedForMs;
}

std::vector<std::string> RelayHealth::Order(const std::vector<std::string>& origins, const int64_t now_ms) const {
  std::vector<std::string> healthy;
  std::vector<std::string> failed;
  std::lock_guard lock(mutex_);
  for (const std::string& origin : origins) {
    (FailedLocked(RelayOriginKey(origin), now_ms) ? failed : healthy).push_back(origin);
  }
  healthy.insert(healthy.end(), failed.begin(), failed.end());
  return healthy;
}

void RelayHealth::NoteFailed(const std::string& origin, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (failed_at_.size() >= kMaxRemembered && !failed_at_.contains(RelayOriginKey(origin))) {
    const auto oldest = std::min_element(failed_at_.begin(), failed_at_.end(),
                                         [](const auto& a, const auto& b) { return a.second < b.second; });
    failed_at_.erase(oldest);
  }
  failed_at_[RelayOriginKey(origin)] = now_ms;
}

void RelayHealth::NoteOk(const std::string& origin) {
  std::lock_guard lock(mutex_);
  failed_at_.erase(RelayOriginKey(origin));
}

bool RelayHealth::FailedRecently(const std::string& origin, const int64_t now_ms) const {
  std::lock_guard lock(mutex_);
  return FailedLocked(RelayOriginKey(origin), now_ms);
}

} // namespace pbr
