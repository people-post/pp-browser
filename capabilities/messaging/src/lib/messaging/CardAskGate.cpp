#include "lib/messaging/CardAskGate.h"

#include <algorithm>

namespace pbr {

bool CardAskGate::Allow(const std::string& account, const int64_t claimed_at, const Source source,
                        const int64_t now_ms) {
  Forget(now_ms);
  const bool version = source == Source::Verified && claimed_at <= now_ms + kCardAtClockSkewMs;
  if (auto it = entries_.find(account); it != entries_.end()) {
    if (it->second.unreadable_at_ms != 0) {
      return false; // Forget cleared it if the gap were over
    }
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
  Bound();
  return true;
}

void CardAskGate::NoteUnreadable(const std::string& account, const int64_t now_ms) {
  Forget(now_ms);
  Entry& entry = entries_[account];
  if (entry.unreadable_at_ms == 0) {
    entry.unreadable_at_ms = now_ms;
  }
  Bound();
}

void CardAskGate::Bound() {
  const auto touched = [](const Entry& entry) { return std::max(entry.asked_at_ms, entry.unreadable_at_ms); };
  while (entries_.size() > kCardAskAccountsMax) {
    auto oldest = std::min_element(entries_.begin(), entries_.end(), [&touched](const auto& a, const auto& b) {
      return touched(a.second) < touched(b.second);
    });
    entries_.erase(oldest);
  }
}

void CardAskGate::Forget(const int64_t now_ms) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    Entry& entry = it->second;
    if (entry.unreadable_at_ms != 0) {
      if (now_ms - entry.unreadable_at_ms < kCardUnreadableReaskGapMs) {
        ++it;
        continue;
      }
      entry.unreadable_at_ms = 0; // over: asked again like any other
    }
    it = now_ms - entry.asked_at_ms >= kCardReaskGapMs ? entries_.erase(it) : std::next(it);
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
