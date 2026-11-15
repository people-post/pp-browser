#pragma once

// TEMPORARY measurement lines for the call-camera lifecycle work (tag `lifecycle.probe`); to be reduced once measured.

#include "foundation/runtime/AppLifecycle.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace pbr::lifecycle_probe {

inline int64_t MonoMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline int64_t WallMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/** Counts `Tick()` runs while a live session is on; one line per second with the count and the largest gap. */
class TickMeter {
public:
  /** UI thread, once per UI loop turn that runs the call tick. */
  void OnTick(const char* app_state) {
    const int64_t now = MonoMs();
    if (last_ms_ != 0) {
      max_gap_ms_ = std::max(max_gap_ms_, now - last_ms_);
    }
    last_ms_ = now;
    ++count_;
    if (window_start_ms_ == 0) {
      window_start_ms_ = now;
    }
    if (now - window_start_ms_ >= 1000) {
      AppLifecycle::logger().info << "lifecycle.probe tick mono_ms=" << now << " wall_ms=" << WallMs()
                                  << " ticks=" << count_ << " max_gap_ms=" << max_gap_ms_ << " ui=" << app_state
                                  << " fg=" << AppLifecycle::IsForeground() << " vis=" << AppLifecycle::IsVisible();
      count_ = 0;
      max_gap_ms_ = 0;
      window_start_ms_ = now;
    }
  }
  /** The live session is off: the next one starts a fresh window. */
  void Reset() {
    last_ms_ = 0;
    window_start_ms_ = 0;
    count_ = 0;
    max_gap_ms_ = 0;
  }

private:
  int64_t last_ms_ = 0;
  int64_t window_start_ms_ = 0;
  int64_t count_ = 0;
  int64_t max_gap_ms_ = 0;
};

} // namespace pbr::lifecycle_probe
