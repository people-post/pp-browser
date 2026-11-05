#pragma once

#include <atomic>
#include <cstdint>

namespace pbr {

/**
 * How often the app runs a frame when nothing wakes it (a posted UI task, input or a timer wakes it
 * at once). Each frame ticks the controllers (e.g. the call screen's expired rings and meters), so
 * these are also their cadence. pp-app-probe paces its frames the same way.
 */
/** While live chrome shows (a ring, a call panel): its pulse and meters stay live. */
inline constexpr double kLiveChromeFrameIntervalSec = 0.55;
/** Idle (power save): the longest wait between frames. */
inline constexpr double kIdleFrameMaxWaitSec = 2.0;
/**
 * A call's video wakes the frame loop itself: the engine calls back for each new self-view or peer frame, and the
 * UI asks for a frame — at most one per this many ms (a camera's 30 fps; a peer frame that comes sooner is drawn
 * with the next). Frames stop, the wakes stop: a camera off, a stalled peer, a minimized call draw nothing extra.
 * Nothing woke the loop for a frame before, so both tiles were drawn only when something else did (0.55 s at most
 * on the call screen) and looked choppy.
 */
inline constexpr int64_t kVideoWakeMinIntervalMs = 30;

/** Whether a video frame at `now_ms` wakes the loop (and if so, records it in `last_wake_ms`). Any thread. */
inline bool TakeVideoFrameWake(std::atomic<int64_t>& last_wake_ms, const int64_t now_ms) {
  int64_t last = last_wake_ms.load(std::memory_order_relaxed);
  while (now_ms - last >= kVideoWakeMinIntervalMs) {
    if (last_wake_ms.compare_exchange_weak(last, now_ms, std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

} // namespace pbr
