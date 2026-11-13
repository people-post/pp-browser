#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace pbr {

/**
 * A call's video wakes the frame loop at most once per this many ms: one frame at 15 fps, the rate every stream
 * is sent at. Several streams out of step used to push the UI to 36-47 frames a second at a 30 ms window
 * (FramePacing's kVideoWakeMinIntervalMs, kept there for its other readers).
 */
inline constexpr int64_t kCallVideoWakeMinIntervalMs = 66;

/**
 * Turns "a new video frame arrived" (any thread: the camera's, a decoder's) into frame-loop wakes. The window's
 * first frame wakes at once; a frame that comes later in the same window is held and woken for when the window
 * ends (one coordinator one-shot per window), so the last frame before a stream stops is never left undrawn.
 *
 * Owned by shared_ptr: the media callback and the one-shot each hold it, so a one-shot firing after the call
 * screen is gone touches only this object (one last wake at most), never the controller.
 */
class CallVideoFrameWake : public std::enable_shared_from_this<CallVideoFrameWake> {
public:
  using Now = std::function<int64_t()>;
  using Schedule = std::function<void(int64_t delay_ms, std::function<void()> fire)>;
  using Wake = std::function<void()>;

  /** The app's clock for the window: monotonic, so a wall-clock time sync never moves it. */
  static int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  static std::shared_ptr<CallVideoFrameWake> Create(Now now, Schedule schedule, Wake wake) {
    return std::shared_ptr<CallVideoFrameWake>(
        new CallVideoFrameWake(std::move(now), std::move(schedule), std::move(wake)));
  }

  /** Any thread: a new frame is ready to draw. */
  void OnFrame() {
    const int64_t now = now_();
    int64_t last = last_wake_ms_.load();
    // A clock that stepped back (an injected one; the app's is monotonic) starts a new window rather than holding frames.
    while (now - last >= kCallVideoWakeMinIntervalMs || now < last) {
      if (last_wake_ms_.compare_exchange_weak(last, now)) {
        held_.store(false); // this wake draws whatever was held back too
        wake_();
        return;
      }
    }
    // Held: the flag before the arm check, so a one-shot that has just cleared `armed_` still sees it.
    held_.store(true);
    if (!armed_.exchange(true)) {
      const int64_t delay = std::max<int64_t>(0, last + kCallVideoWakeMinIntervalMs - now);
      schedule_(delay, [self = shared_from_this()] { self->OnWindowEnd(); });
    }
  }

private:
  CallVideoFrameWake(Now now, Schedule schedule, Wake wake)
      : now_(std::move(now)), schedule_(std::move(schedule)), wake_(std::move(wake)) {}

  void OnWindowEnd() {
    armed_.store(false); // before reading `held_`: a frame held from here on arms a new one-shot
    if (held_.exchange(false)) {
      last_wake_ms_.store(now_()); // the catch-up wake starts the next window
      wake_();
    }
  }

  const Now now_;
  const Schedule schedule_;
  const Wake wake_;
  std::atomic<int64_t> last_wake_ms_{INT64_MIN / 2};
  std::atomic<bool> held_{false};  // a frame in the current window has not been woken for
  std::atomic<bool> armed_{false}; // a window-end one-shot is scheduled
};

} // namespace pbr
