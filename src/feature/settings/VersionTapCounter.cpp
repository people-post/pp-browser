#include "feature/settings/VersionTapCounter.h"

namespace pbr {

VersionTapCounter::Result VersionTapCounter::Tap(const uint64_t now_ms) {
  if (count_ > 0 && now_ms - last_ms_ > kMaxGapMs) {
    count_ = 0;
  }
  ++count_;
  last_ms_ = now_ms;
  if (count_ >= kTapsNeeded) {
    count_ = 0;
    return {Action::Toggle, 0};
  }
  if (count_ >= kHintFromTap) {
    return {Action::Hint, kTapsNeeded - count_};
  }
  return {};
}

} // namespace pbr
