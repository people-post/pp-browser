#pragma once

#include <chrono>

namespace pbr {

/**
 * iPad window controls: the inset that clears them (WindowControlsTopInset) comes from UIKit's corner adaptation, which
 * settles some frames after the window is laid out and raises no SDL event when it does (view.safeAreaInsets, which
 * SDL watches, does not change). The shell reads its insets on a frame, and an idle app has none, so a first read taken
 * too early left the rail under the controls until something else woke the app. This asks for frames while the window's
 * size or top inset is new, and for a short while after the last change.
 */
class SafeAreaSettle {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kSettle{2500};

  /** The window size and top inset read this frame. true while frames should keep coming. */
  bool Observe(Clock::time_point now, int window_w, int window_h, int top_dp) {
    if (!seen_ || window_w != window_w_ || window_h != window_h_ || top_dp != top_dp_) {
      seen_ = true;
      window_w_ = window_w;
      window_h_ = window_h;
      top_dp_ = top_dp;
      until_ = now + kSettle;
    }
    return now < until_;
  }

private:
  bool seen_ = false;
  int window_w_ = 0;
  int window_h_ = 0;
  int top_dp_ = 0;
  Clock::time_point until_{};
};

} // namespace pbr
