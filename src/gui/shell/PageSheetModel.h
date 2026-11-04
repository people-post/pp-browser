#pragma once

#include "lib/ui/ShellTypes.h"

#include <deque>

namespace pbr {

/**
 * Phone page sheet (iOS style, Kenneth 2026-11-04): the swipe-down decisions without any DOM. A drag down by a
 * quarter of the sheet's height closes it, and so does a flick; anything less springs back.
 *
 * A sheet with a half stop (the ask-AI panel, DEV/PLAN-ui-batch5-ask-ai.md 5a) moves between two heights: from half,
 * up by a quarter of its height (or a flick up) goes full, down by a quarter (or a flick down) closes; from full, the
 * same drag down goes back to half instead of closing.
 */
struct PageSheetModel {
  static constexpr float kDismissRatio = 0.25f;
  /** A release this fast (downwards) closes the sheet even when it has moved less than the quarter... */
  static constexpr float kFlickVelocityDpPerS = 700.f;
  /** ...once it has moved at least this far (a tap that wobbles is not a flick). */
  static constexpr float kFlickMinDragDp = 24.f;
  /** Moves older than this at release do not count towards the flick speed (a drag that stopped is no flick). */
  static constexpr double kVelocityWindowMs = 100.0;

  /** What a released drag does to the sheet. */
  enum class Release { Stay, ToFull, ToHalf, Dismiss };

  /**
   * `drag_dp` is downwards (up is negative), `height_dp` the height the sheet shows now, `velocity_dp_per_s`
   * downwards too. Without a half stop only Stay and Dismiss come back.
   */
  static Release OnRelease(const SheetDetent detent, const bool half_stop, const float drag_dp, const float height_dp,
                           const float velocity_dp_per_s) {
    if (!half_stop) {
      return ShouldDismiss(drag_dp, height_dp, velocity_dp_per_s) ? Release::Dismiss : Release::Stay;
    }
    if (ShouldDismiss(drag_dp, height_dp, velocity_dp_per_s)) {
      return detent == SheetDetent::Full ? Release::ToHalf : Release::Dismiss;
    }
    if (detent == SheetDetent::Half && ShouldDismiss(-drag_dp, height_dp, -velocity_dp_per_s)) {
      return Release::ToFull;
    }
    return Release::Stay;
  }

  /** The height a sheet shows: full while the keyboard is up (the answers need the room), else where it stands. */
  static SheetDetent Shown(const SheetDetent detent, const bool keyboard_up) {
    return keyboard_up ? SheetDetent::Full : detent;
  }

  static bool ShouldDismiss(const float drag_dp, const float sheet_height_dp, const float velocity_dp_per_s) {
    if (drag_dp <= 0.f) {
      return false;
    }
    if (sheet_height_dp > 0.f && drag_dp >= sheet_height_dp * kDismissRatio) {
      return true;
    }
    return velocity_dp_per_s >= kFlickVelocityDpPerS && drag_dp >= kFlickMinDragDp;
  }

  /** Downward speed at release from the recent moves (dp per second; up is negative). */
  class Velocity {
  public:
    void Reset() { samples_.clear(); }
    void Add(const double time_ms, const float y_dp) {
      samples_.push_back({time_ms, y_dp});
      while (samples_.size() > 2 && time_ms - samples_.front().time_ms > kVelocityWindowMs) {
        samples_.pop_front();
      }
    }
    float AtRelease(const double release_ms) const {
      if (samples_.size() < 2 || release_ms - samples_.back().time_ms > kVelocityWindowMs) {
        return 0.f;
      }
      const Sample& first = samples_.front();
      const Sample& last = samples_.back();
      const double span_ms = last.time_ms - first.time_ms;
      if (span_ms <= 0.0) {
        return 0.f;
      }
      return static_cast<float>((last.y_dp - first.y_dp) * 1000.0 / span_ms);
    }

  private:
    struct Sample {
      double time_ms = 0.0;
      float y_dp = 0.f;
    };
    std::deque<Sample> samples_;
  };
};

} // namespace pbr
