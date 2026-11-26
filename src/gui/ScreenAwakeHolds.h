#pragma once

#include "common/PbrCompat.h"
#include "foundation/platform/ScreenAwake.h"

#include <array>
#include <functional>
#include <utility>

namespace pbr {

/**
 * Who wants the screen kept on. The OS switch (ScreenAwake) is one global flag, so two holders (a call, a forward's
 * progress page) must not each flip it: releasing one holder's hold leaves the other's in place, and the OS switch
 * changes only when "somebody holds" changes. UI thread.
 */
class ScreenAwakeHolds {
public:
  enum class Holder { Call, Forward, Count };

  /** `apply(on, reason)` is the OS switch; the default is ScreenAwake::SetKeepScreenOn. */
  explicit ScreenAwakeHolds(std::function<void(bool on, const char* reason)> apply = {}) : apply_(std::move(apply)) {}

  /** The process-wide one the call controller and the forward page share. */
  static ScreenAwakeHolds& Shared() {
    static ScreenAwakeHolds shared;
    return shared;
  }

  /** Sets `holder`'s hold. True when the OS switch was called (the any-holder state changed). */
  bool Set(const Holder holder, const bool on, const char* reason) {
    const bool before = Any();
    held_[static_cast<size_t>(holder)] = on;
    const bool after = Any();
    if (before == after) {
      return false;
    }
    if (apply_) {
      apply_(after, reason);
    } else {
      ScreenAwake::SetKeepScreenOn(after, reason);
    }
    return true;
  }

  bool Held(const Holder holder) const { return held_[static_cast<size_t>(holder)]; }
  bool Any() const {
    for (const bool held : held_) {
      if (held) {
        return true;
      }
    }
    return false;
  }

private:
  std::function<void(bool, const char*)> apply_;
  std::array<bool, static_cast<size_t>(Holder::Count)> held_{};
};

} // namespace pbr
