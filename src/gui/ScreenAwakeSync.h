#pragma once

#include "common/PbrCompat.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace pbr {

/**
 * Hands the screen-awake switch a value only when it changes, like SyncOngoingCall's old / new compare, so the per-frame
 * tick costs nothing. It starts "off", which matches the OS state BrowserHost sets once video is up
 * (ScreenAwake::SetKeepScreenOn(false, "startup")). Turning on is immediate; turning off waits kOffDebounceMs of
 * uninterrupted "off" requests, so a peer's picture that flaps on a bad link does not flip the switch each time.
 * `ForceOff` is for the call's end and the controller's teardown, and is immediate. Time is passed in. UI thread.
 */
class ScreenAwakeSync {
public:
  static constexpr int64_t kOffDebounceMs = 2000;

  explicit ScreenAwakeSync(std::function<void(bool)> set) : set_(std::move(set)) {}

  /** True when the switch was called. */
  bool Set(const bool on, const int64_t now_ms) {
    if (on) {
      off_since_ms_.reset();
      return Switch(true);
    }
    if (!on_) {
      off_since_ms_.reset();
      return false;
    }
    if (!off_since_ms_) {
      off_since_ms_ = now_ms;
    }
    if (now_ms - *off_since_ms_ < kOffDebounceMs) {
      return false;
    }
    return Switch(false);
  }

  bool ForceOff() {
    off_since_ms_.reset();
    return Switch(false);
  }

  bool On() const { return on_; }

private:
  bool Switch(const bool on) {
    if (on == on_) {
      return false;
    }
    on_ = on;
    off_since_ms_.reset();
    set_(on);
    return true;
  }

  std::function<void(bool)> set_;
  bool on_ = false;
  std::optional<int64_t> off_since_ms_;
};

} // namespace pbr
