#pragma once

#include "gui/shell/AiDotModel.h"

#include <cstdint>
#include <optional>

namespace pbr {

/**
 * The AI Home's mascot without any DOM (Kenneth 2026-11-09): still until it is tapped; a tap plays the poke, then one
 * random action of its look (the dot's keyframes, AiDotModel), then it is still again. Nothing runs between taps, so
 * nothing wakes the app. A tap while an action runs is ignored. Times are milliseconds on any steady clock.
 */
class HomeMascotModel {
public:
  /** A tap at `now_ms`; `roll` is any random number. False when an action is already playing. */
  bool Tap(const int64_t now_ms, const AiDotLook look, const uint32_t roll) {
    if (idle_ != AiDotIdle::None) {
      return false;
    }
    look_ = look;
    roll_ = roll;
    Start(AiDotIdle::Poke, now_ms);
    return true;
  }

  /** Moves the action on; true when Idle() changed. */
  bool Advance(const int64_t now_ms) {
    if (idle_ == AiDotIdle::None || now_ms < end_ms_) {
      return false;
    }
    if (idle_ == AiDotIdle::Poke) {
      Start(AiDotModel::PickIdle(look_, AiDotIdle::Poke, roll_), now_ms);
    } else {
      idle_ = AiDotIdle::None;
    }
    return true;
  }

  /** Milliseconds until Advance has something to do; nullopt while still. */
  std::optional<int64_t> MsUntilNext(const int64_t now_ms) const {
    if (idle_ == AiDotIdle::None) {
      return std::nullopt;
    }
    return end_ms_ > now_ms ? end_ms_ - now_ms : 0;
  }

  AiDotIdle Idle() const { return idle_; }

private:
  void Start(const AiDotIdle idle, const int64_t now_ms) {
    idle_ = idle;
    end_ms_ = now_ms + AiDotModel::IdleLengthMs(idle, look_) + AiDotModel::kIdleTailMs;
  }

  AiDotIdle idle_ = AiDotIdle::None;
  AiDotLook look_ = AiDotLook::TwoEyes;
  uint32_t roll_ = 0;
  int64_t end_ms_ = 0;
};

} // namespace pbr
