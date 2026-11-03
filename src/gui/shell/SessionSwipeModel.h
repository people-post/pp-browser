#pragma once

#include "lib/ui/ShellGestureAxis.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace pbr {

/** What a swipe button does; ChatMenus maps each onto the long-press menu item it mirrors. */
enum class SessionSwipeAction { Archive, Delete, Pin, Read };

/** Which side of the row the buttons sit on: End = row slid left (buttons on the right), Start = slid right. */
enum class SwipeSide { None, Start, End };

struct SessionSwipeConfig {
  float button_dp = 74.f;
  int end_buttons = 2;
  int start_buttons = 2;
  /** A release past this share of the row width runs the side's first action. */
  float full_ratio = 0.6f;
  int deadzone_px = 8;
};

struct SessionSwipeOutcome {
  enum class Kind { None, Close, Open, RunFirst };
  Kind kind = Kind::None;
  SwipeSide side = SwipeSide::None;
  /** The click that follows this release belongs to the gesture, not to the row. */
  bool swallow_click = false;
};

/**
 * The swipe-row decisions without any DOM: axis lock, row offset, open / close / full-swipe outcome and
 * "only one row open". Positions are pixels; `dpr` converts them to dp. UI thread.
 */
class SessionSwipeModel {
public:
  struct Move {
    bool claimed = false;
    float offset_dp = 0.f; // negative = End side
  };

  explicit SessionSwipeModel(SessionSwipeConfig config = {}) : config_(config) {}

  float OpenWidthDp(SwipeSide side) const {
    if (side == SwipeSide::End) {
      return config_.button_dp * static_cast<float>(config_.end_buttons);
    }
    if (side == SwipeSide::Start) {
      return config_.button_dp * static_cast<float>(config_.start_buttons);
    }
    return 0.f;
  }

  /** A press on `row_id`. Returns the id of a different row that was open and is now closed (else empty). */
  std::string Press(const std::string& row_id, int x_px, int y_px, float row_width_dp) {
    std::string closed;
    pressed_ = true;
    dragging_ = false;
    closed_other_ = false;
    row_ = row_id;
    row_width_dp_ = row_width_dp;
    start_x_ = x_px;
    start_y_ = y_px;
    axis_.Unlock();
    pressed_open_ = !open_row_.empty() && open_row_ == row_id;
    if (!open_row_.empty() && !pressed_open_) {
      closed = open_row_;
      Clear();
      closed_other_ = true;
    }
    base_dp_ = pressed_open_ ? (open_side_ == SwipeSide::End ? -OpenWidthDp(open_side_) : OpenWidthDp(open_side_)) : 0.f;
    offset_dp_ = base_dp_;
    return closed;
  }

  Move Update(int x_px, int y_px, float dpr) {
    if (!pressed_) {
      return {};
    }
    const int dx = x_px - start_x_;
    const int dy = y_px - start_y_;
    if (!dragging_) {
      const ShellGestureAxis axis = axis_.Observe(dx, dy, config_.deadzone_px);
      if (axis == ShellGestureAxis::None) {
        return {};
      }
      if (axis == ShellGestureAxis::Vertical) {
        pressed_ = false; // the list scrolls; this row never opens
        return {};
      }
      dragging_ = true;
    }
    const float ratio = dpr > 0.f ? dpr : 1.f;
    const float min_dp = config_.end_buttons > 0 ? -row_width_dp_ : 0.f;
    const float max_dp = config_.start_buttons > 0 ? row_width_dp_ : 0.f;
    offset_dp_ = std::clamp(base_dp_ + static_cast<float>(dx) / ratio, min_dp, max_dp);
    return {true, offset_dp_};
  }

  SessionSwipeOutcome Release() {
    SessionSwipeOutcome out;
    if (!pressed_) {
      return out;
    }
    pressed_ = false;
    if (!dragging_) {
      // A tap on an open row, or anywhere while another row was open, only closes: it does not open the chat.
      if (pressed_open_ || closed_other_) {
        out.kind = SessionSwipeOutcome::Kind::Close;
        out.swallow_click = true;
        Clear();
      }
      return out;
    }
    dragging_ = false;
    out.swallow_click = true;
    const SwipeSide side = offset_dp_ < 0.f ? SwipeSide::End : offset_dp_ > 0.f ? SwipeSide::Start : SwipeSide::None;
    const float mag = std::fabs(offset_dp_);
    out.side = side;
    if (side != SwipeSide::None && mag >= row_width_dp_ * config_.full_ratio) {
      out.kind = SessionSwipeOutcome::Kind::RunFirst;
      Clear();
    } else if (side != SwipeSide::None && mag >= OpenWidthDp(side) * 0.5f) {
      out.kind = SessionSwipeOutcome::Kind::Open;
      open_row_ = row_;
      open_side_ = side;
    } else {
      out.kind = SessionSwipeOutcome::Kind::Close;
      out.side = SwipeSide::None;
      Clear();
    }
    return out;
  }

  /** Scroll / external close. Returns the row that was open (empty if none). */
  std::string CloseOpen() {
    std::string closed = open_row_;
    Clear();
    return closed;
  }

  const std::string& OpenRow() const { return open_row_; }
  SwipeSide OpenSide() const { return open_side_; }
  bool Dragging() const { return dragging_; }
  /** The resting offset of the open row (dp, negative = End). */
  float RestOffsetDp(SwipeSide side) const {
    return side == SwipeSide::End ? -OpenWidthDp(side) : side == SwipeSide::Start ? OpenWidthDp(side) : 0.f;
  }

private:
  void Clear() {
    open_row_.clear();
    open_side_ = SwipeSide::None;
  }

  SessionSwipeConfig config_;
  ShellGestureAxisLock axis_;
  std::string open_row_;
  SwipeSide open_side_ = SwipeSide::None;
  std::string row_;
  float row_width_dp_ = 0.f;
  float base_dp_ = 0.f;
  float offset_dp_ = 0.f;
  int start_x_ = 0;
  int start_y_ = 0;
  bool pressed_ = false;
  bool dragging_ = false;
  bool pressed_open_ = false;
  bool closed_other_ = false;
};

} // namespace pbr
