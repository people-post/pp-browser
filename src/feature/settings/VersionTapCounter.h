#pragma once

#include <cstdint>

namespace pbr {

/**
 * Five quick taps on the version row turn "advanced settings" on; five more turn it off (DEV/PLAN-settings-reorg.md,
 * "连点 5 次的唯一定义"). A gap of more than two seconds between two taps starts the count again. From the third tap on
 * the user is told how many taps are left; the fifth tap toggles.
 */
class VersionTapCounter {
public:
  static constexpr int kTapsNeeded = 5;
  static constexpr int kHintFromTap = 3;
  static constexpr uint64_t kMaxGapMs = 2000;

  enum class Action { None, Hint, Toggle };
  struct Result {
    Action action = Action::None;
    /** Taps still needed (Hint only). */
    int taps_left = 0;
  };

  /** One tap at `now_ms` (any monotonic millisecond clock). */
  Result Tap(uint64_t now_ms);

private:
  int count_ = 0;
  uint64_t last_ms_ = 0;
};

} // namespace pbr
