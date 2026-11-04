#pragma once

#include "lib/ui/ShellTypes.h"

#include <deque>

namespace pbr {

/**
 * Phone page sheet (iOS style, Kenneth 2026-11-04): the drag decisions without any DOM. Every sheet opens at Large
 * (about 4/5 of the screen) and stops at three heights: from Large a drag up goes Full and a drag down goes Half; from
 * Full a drag down goes back to Large; from Half a drag down closes it (and a drag up goes back to Large). A drag
 * moves on to the next stop once it has covered a quarter of the way there (closing: a quarter of the sheet's height
 * at half), or when it is flicked; anything less springs back. A long drag can pass more than one stop.
 *
 * The tops here are the ones components.rcss gives .shell-page-sheet and its --large / --half classes.
 */
struct PageSheetModel {
  /** The top edge at each stop: Full a fixed gap under the status bar, the others a share of the sheet layer. */
  static constexpr float kFullTopDp = 10.f;
  static constexpr float kLargeTopRatio = 0.2f;
  static constexpr float kHalfTopRatio = 0.45f;
  /** The share of the way to the next stop a slow drag must cover. */
  static constexpr float kMoveRatio = 0.25f;
  /** A release this fast (towards the next stop) moves there even when it has covered less than the quarter... */
  static constexpr float kFlickVelocityDpPerS = 700.f;
  /** ...once it has moved at least this far (a tap that wobbles is not a flick). */
  static constexpr float kFlickMinDragDp = 24.f;
  /** Moves older than this at release do not count towards the flick speed (a drag that stopped is no flick). */
  static constexpr double kVelocityWindowMs = 100.0;

  /** What a released drag does to the sheet. */
  enum class Release { Stay, ToFull, ToLarge, ToHalf, Dismiss };

  /** The sheet's top edge at `detent`, in dp from the top of a layer `layer_height_dp` tall. */
  static float TopDp(const SheetDetent detent, const float layer_height_dp) {
    switch (detent) {
    case SheetDetent::Full:
      return kFullTopDp;
    case SheetDetent::Large:
      return layer_height_dp * kLargeTopRatio;
    case SheetDetent::Half:
      return layer_height_dp * kHalfTopRatio;
    }
    return kFullTopDp;
  }

  /**
   * `from` is the stop the sheet showed at the press, `drag_dp` and `velocity_dp_per_s` are downwards (up is
   * negative), `layer_height_dp` the height of the layer the sheet stands in.
   */
  static Release OnRelease(const SheetDetent from, const float drag_dp, const float layer_height_dp,
                           const float velocity_dp_per_s) {
    const float top = TopDp(from, layer_height_dp) + drag_dp;
    SheetDetent at = from;
    if (drag_dp > 0.f) {
      while (true) {
        const float at_top = TopDp(at, layer_height_dp);
        // Below Half the sheet closes: the way there is the sheet's height at half.
        const float way = at == SheetDetent::Half ? layer_height_dp - at_top
                                                  : TopDp(Below(at), layer_height_dp) - at_top;
        if (!Passes(top - at_top, way, at == from ? velocity_dp_per_s : 0.f)) {
          break;
        }
        if (at == SheetDetent::Half) {
          return Release::Dismiss;
        }
        at = Below(at);
      }
    } else if (drag_dp < 0.f) {
      while (at != SheetDetent::Full) {
        const float at_top = TopDp(at, layer_height_dp);
        const float way = at_top - TopDp(Above(at), layer_height_dp);
        if (!Passes(at_top - top, way, at == from ? -velocity_dp_per_s : 0.f)) {
          break;
        }
        at = Above(at);
      }
    }
    if (at == from) {
      return Release::Stay;
    }
    return at == SheetDetent::Full ? Release::ToFull : at == SheetDetent::Large ? Release::ToLarge : Release::ToHalf;
  }

  /** The height a sheet shows: full while the keyboard is up (the content needs the room), else where it stands. */
  static SheetDetent Shown(const SheetDetent detent, const bool keyboard_up) {
    return keyboard_up ? SheetDetent::Full : detent;
  }

  /** `moved_dp` towards a stop `way_dp` away, released at `velocity_dp_per_s` towards it: does the sheet go there? */
  static bool Passes(const float moved_dp, const float way_dp, const float velocity_dp_per_s) {
    if (moved_dp <= 0.f) {
      return false;
    }
    if (way_dp > 0.f && moved_dp >= way_dp * kMoveRatio) {
      return true;
    }
    return velocity_dp_per_s >= kFlickVelocityDpPerS && moved_dp >= kFlickMinDragDp;
  }

  static SheetDetent Below(const SheetDetent detent) {
    return detent == SheetDetent::Full ? SheetDetent::Large : SheetDetent::Half;
  }
  static SheetDetent Above(const SheetDetent detent) {
    return detent == SheetDetent::Half ? SheetDetent::Large : SheetDetent::Full;
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
