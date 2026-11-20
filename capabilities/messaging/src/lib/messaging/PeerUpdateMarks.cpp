#include "lib/messaging/PeerUpdateMarks.h"

#include <algorithm>

namespace pbr {

bool PeerUpdateMarks::Note(const std::string& account, const int64_t now_ms) {
  if (account.empty()) {
    return false;
  }
  auto [it, fresh] = marks_.try_emplace(account);
  Mark& mark = it->second;
  mark.seen_at_ms = now_ms;
  const bool tell = fresh || now_ms - mark.told_at_ms >= kNeedsUpdateNoticeGapMs;
  if (tell) {
    mark.told_at_ms = now_ms;
  }
  while (marks_.size() > kPeerUpdateMarksMax) {
    auto oldest = std::min_element(marks_.begin(), marks_.end(), [](const auto& a, const auto& b) {
      return a.second.seen_at_ms < b.second.seen_at_ms;
    });
    marks_.erase(oldest);
  }
  return tell;
}

} // namespace pbr
