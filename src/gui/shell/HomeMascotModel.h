#pragma once

#include "gui/shell/AiDotModel.h"

#include <cstdint>
#include <optional>

namespace pbr {

/**
 * The AI Home's mascot without any DOM (Kenneth 2026-11-09): still until it is tapped; a tap plays the poke, then one
 * random action of its look (the dot's keyframes, AiDotModel), then it is still again. While the Home is on screen and
 * quiet (SetActive) it also plays one random action by itself every kIdleMinMs to kIdleMaxMs; a tap and the end of an
 * action start that wait again. Nothing runs between actions, so the only wake is the next one. A tap while an action
 * runs is ignored. Times are milliseconds on any steady clock.
 */
class HomeMascotModel {
public:
  /** The pause before the mascot moves by itself: sparser than the floating dot's 3-6 s. */
  static constexpr int64_t kIdleMinMs = 10000;
  static constexpr int64_t kIdleMaxMs = 20000;
  /** A planned action this much overdue was missed (the app was suspended or stalled): wait again instead. */
  static constexpr int64_t kMissedMs = 2000;

  static int64_t IdleDelayMs(const uint32_t roll) {
    return kIdleMinMs + static_cast<int64_t>(roll % static_cast<uint32_t>(kIdleMaxMs - kIdleMinMs + 1));
  }

  /**
   * Whether the Home is on screen with nothing over it (no sheet, panel, call or keyboard) and the app in front: it
   * then moves by itself from time to time. Going active starts the wait; going inactive cancels it (a running action
   * finishes).
   */
  void SetActive(const int64_t now_ms, const bool active, const uint32_t roll) {
    if (active == active_) {
      return;
    }
    active_ = active;
    next_idle_ms_ = active ? std::optional<int64_t>(now_ms + IdleDelayMs(roll)) : std::nullopt;
  }

  /** A tap at `now_ms`; `roll` is any random number. False when an action is already playing. */
  bool Tap(const int64_t now_ms, const AiDotLook look, const uint32_t roll) {
    if (idle_ != AiDotIdle::None) {
      return false;
    }
    look_ = look;
    roll_ = roll;
    next_idle_ms_.reset();
    Start(AiDotIdle::Poke, now_ms);
    return true;
  }

  /** Moves the action on; true when Idle() changed. `look` and `roll` pick and time the next action by itself. */
  bool Advance(const int64_t now_ms, const AiDotLook look, const uint32_t roll) {
    if (idle_ == AiDotIdle::None) {
      if (!active_ || !next_idle_ms_ || now_ms < *next_idle_ms_) {
        return false;
      }
      if (now_ms - *next_idle_ms_ > kMissedMs) {
        next_idle_ms_ = now_ms + IdleDelayMs(roll);
        return false;
      }
      look_ = look;
      next_idle_ms_.reset();
      Start(AiDotModel::PickIdle(look_, AiDotIdle::Poke, roll), now_ms);
      return true;
    }
    if (now_ms < end_ms_) {
      return false;
    }
    if (idle_ == AiDotIdle::Poke) {
      Start(AiDotModel::PickIdle(look_, AiDotIdle::Poke, roll_), now_ms);
    } else {
      idle_ = AiDotIdle::None;
      if (active_) {
        next_idle_ms_ = now_ms + IdleDelayMs(roll);
      }
    }
    return true;
  }

  /** Milliseconds until Advance has something to do; nullopt while still and nothing is planned. */
  std::optional<int64_t> MsUntilNext(const int64_t now_ms) const {
    if (idle_ == AiDotIdle::None) {
      if (!active_ || !next_idle_ms_) {
        return std::nullopt;
      }
      return *next_idle_ms_ > now_ms ? *next_idle_ms_ - now_ms : 0;
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
  bool active_ = false;
  std::optional<int64_t> next_idle_ms_;
};

} // namespace pbr
