#pragma once

#include "common/PbrCompat.h"

#include <functional>
#include <utility>

namespace pbr {

/**
 * Hands the screen-awake switch a value only when it changes (initially off), like SyncOngoingCall's old / new
 * compare, so the per-frame tick costs nothing. `ForceOff` is for the call's end and the controller's teardown.
 * UI thread.
 */
class ScreenAwakeSync {
public:
  explicit ScreenAwakeSync(std::function<void(bool)> set) : set_(std::move(set)) {}

  /** True when the switch was called. */
  bool Set(const bool on) {
    if (on == on_) {
      return false;
    }
    on_ = on;
    set_(on);
    return true;
  }

  bool ForceOff() { return Set(false); }

  bool On() const { return on_; }

private:
  std::function<void(bool)> set_;
  bool on_ = false;
};

} // namespace pbr
