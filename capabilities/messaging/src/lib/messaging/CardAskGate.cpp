#include "lib/messaging/CardAskGate.h"

#include <algorithm>

namespace pbr {

bool CardAskGate::Allow(const std::string& account, const int64_t claimed_at, const Source source,
                        const int64_t now_ms) {
  Forget(now_ms);
  const bool version = source == Source::Verified && claimed_at <= now_ms + kCardAtClockSkewMs;
  if (auto it = entries_.find(account); it != entries_.end()) {
    const bool newer = version && claimed_at > it->second.asked_for;
    if (!newer && now_ms - it->second.asked_at_ms < kCardReaskGapMs) {
      return false;
    }
  }
  if (source == Source::Unverified && !TakeUnverifiedSlot(now_ms)) {
    return false;
  }
  Entry& entry = entries_[account];
  entry.asked_at_ms = now_ms;
  if (version) {
    entry.asked_for = std::max(entry.asked_for, claimed_at);
  }
  while (entries_.size() > kCardAskAccountsMax) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
      return a.second.asked_at_ms < b.second.asked_at_ms;
    });
    entries_.erase(oldest);
  }
  return true;
}

void CardAskGate::Forget(const int64_t now_ms) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    it = now_ms - it->second.asked_at_ms >= kCardReaskGapMs ? entries_.erase(it) : std::next(it);
  }
  while (!unverified_asks_ms_.empty() && now_ms - unverified_asks_ms_.front() >= kUnverifiedCardAskWindowMs) {
    unverified_asks_ms_.pop_front();
  }
}

bool CardAskGate::TakeUnverifiedSlot(const int64_t now_ms) {
  if (unverified_asks_ms_.size() >= kUnverifiedCardAsksPerWindow) {
    return false;
  }
  unverified_asks_ms_.push_back(now_ms);
  return true;
}

} // namespace pbr
