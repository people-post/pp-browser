#pragma once

#include "lib/ui/ShellTypes.h"

#include <cmath>
#include <deque>
#include <optional>

namespace pbr {

/**
 * Phone page sheet (iOS style, Kenneth 2026-11-04): the drag decisions without any DOM. Every sheet opens at Large
 * (about 4/5 of the screen) and stops at four heights, Full, Large, Half and Small (about a third of the screen: the
 * title row and the top of the content). Like Apple Maps, a released sheet goes to the stop nearest its top edge; a
 * flick goes on to the next stop in the flick's direction instead. It closes only when it is let go with its top
 * below 85% of the layer, or flicked down from Small.
 *
 * The tops here are the ones components.rcss gives .shell-page-sheet and its --large / --half / --small classes.
 */
struct PageSheetModel {
  /** The top edge at each stop: Full a fixed gap under the status bar, the others a share of the sheet layer. */
  static constexpr float kFullTopDp = 10.f;
  static constexpr float kLargeTopRatio = 0.2f;
  static constexpr float kHalfTopRatio = 0.45f;
  static constexpr float kSmallTopRatio = 0.66f;
  /** Let go with its top below this share of the layer, the sheet closes. */
  static constexpr float kDismissTopRatio = 0.85f;
  /** A release this fast moves one stop on in its direction... */
  static constexpr float kFlickVelocityDpPerS = 700.f;
  /** ...once it has moved at least this far (a tap that wobbles is not a flick). */
  static constexpr float kFlickMinDragDp = 24.f;
  /** Moves older than this at release do not count towards the flick speed (a drag that stopped is no flick). */
  static constexpr double kVelocityWindowMs = 100.0;

  /** What a released drag does to the sheet. */
  enum class Release { Stay, ToFull, ToLarge, ToHalf, ToSmall, Dismiss };

  /** The sheet's top edge at `detent`, in dp from the top of a layer `layer_height_dp` tall. */
  static float TopDp(const SheetDetent detent, const float layer_height_dp) {
    switch (detent) {
      case SheetDetent::Full:
        return kFullTopDp;
      case SheetDetent::Large:
        return layer_height_dp * kLargeTopRatio;
      case SheetDetent::Half:
        return layer_height_dp * kHalfTopRatio;
      case SheetDetent::Small:
        return layer_height_dp * kSmallTopRatio;
    }
    return kFullTopDp;
  }

  /**
   * `from` is the stop the sheet showed at the press, `drag_dp` and `velocity_dp_per_s` are downwards (up is
   * negative), `layer_height_dp` the height of the layer the sheet stands in.
   */
  static Release OnRelease(const SheetDetent from, const float drag_dp, const float layer_height_dp,
                           const float velocity_dp_per_s) {
    static constexpr SheetDetent kTopDown[] = {SheetDetent::Full, SheetDetent::Large, SheetDetent::Half,
                                               SheetDetent::Small};
    const float top = TopDp(from, layer_height_dp) + drag_dp;
    const bool flick = std::abs(velocity_dp_per_s) >= kFlickVelocityDpPerS && std::abs(drag_dp) >= kFlickMinDragDp;
    std::optional<SheetDetent> to;
    if (flick && velocity_dp_per_s > 0.f) {
      // Down: the first stop below the top edge. Past Small, only a sheet that stood at Small (or is let go below
      // the closing line) closes; one flicked down from higher up stops at Small.
      for (const SheetDetent detent : kTopDown) {
        if (TopDp(detent, layer_height_dp) > top) {
          to = detent;
          break;
        }
      }
      if (!to) {
        if (from == SheetDetent::Small || top >= layer_height_dp * kDismissTopRatio) {
          return Release::Dismiss;
        }
        to = SheetDetent::Small;
      }
    } else if (flick) {
      // Up: the first stop above the top edge; nothing above Full.
      to = SheetDetent::Full;
      for (const SheetDetent detent : kTopDown) {
        if (TopDp(detent, layer_height_dp) < top) {
          to = detent;
        }
      }
    } else if (top >= layer_height_dp * kDismissTopRatio) {
      return Release::Dismiss;
    } else {
      to = SheetDetent::Full;
      for (const SheetDetent detent : kTopDown) {
        if (std::abs(TopDp(detent, layer_height_dp) - top) < std::abs(TopDp(*to, layer_height_dp) - top)) {
          to = detent;
        }
      }
    }
    if (*to == from) {
      return Release::Stay;
    }
    switch (*to) {
      case SheetDetent::Full:
        return Release::ToFull;
      case SheetDetent::Large:
        return Release::ToLarge;
      case SheetDetent::Half:
        return Release::ToHalf;
      case SheetDetent::Small:
        return Release::ToSmall;
    }
    return Release::Stay;
  }

  /** The height a sheet shows: full while the keyboard is up (the content needs the room), else where it stands. */
  static SheetDetent Shown(const SheetDetent detent, const bool keyboard_up) {
    return keyboard_up ? SheetDetent::Full : detent;
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
