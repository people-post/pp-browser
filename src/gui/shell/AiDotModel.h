#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace pbr {

/** What is on screen, for deciding whether the phone AI dot shows (DESIGN-pp-ui §8.14, PLAN-ui-batch5-ask-ai 5c). */
struct AiDotScene {
  bool enabled = true;     ///< Me → AI assistant → Show AI dot.
  bool phone = true;       ///< Compact layout; desktop has no floating dot in this batch.
  bool call = false;       ///< An incoming ring, a call on screen, or its bar.
  bool pin_gate = false;   ///< The PIN gate (unlock, create, the first-run identity fork).
  bool cover = false;      ///< The startup cover or the onboarding wizard.
  bool secret_page = false;///< Me → Account & security: change PIN, transfer, the recovery phrase.
  bool scanning = false;   ///< The full-screen QR scanner.
  bool layer_open = false; ///< Any layer over the page: a page sheet (the ask-AI panel too) or a panel.
  bool keyboard_up = false;///< The on-screen keyboard or the emoji panel.
};

/** The short idle move the dot makes now and then (RCSS keyframes; nothing runs between them). */
enum class AiDotIdle { None, Sway, Blink };

/** Where the dot rests: the side it sticks to and how far down the band it stands (0 top, 1000 bottom). */
struct AiDotPlace {
  bool left = false;
  int y_permille = 600;

  bool operator==(const AiDotPlace& other) const { return left == other.left && y_permille == other.y_permille; }
  bool operator!=(const AiDotPlace& other) const { return !(*this == other); }
};

/** The screen the dot lives on, in dp: the whole document body (edge to edge on phones) and its safe areas. */
struct AiDotArea {
  float width_dp = 0.f;
  float height_dp = 0.f;
  float safe_top_dp = 0.f;
  float safe_bottom_dp = 0.f;
};

struct AiDotPoint {
  float x_dp = 0.f;
  float y_dp = 0.f;
};

/**
 * The phone AI dot without any DOM (Kenneth 2026-11-02/03: "可以到处放，或者把它吸在侧边", "设置里面设置打开或关闭"):
 * dragged anywhere, on release it sticks to the nearer left or right edge inside the safe area; 4.5 s after the last
 * touch it tucks half off the screen, a press brings it out again; while it shows, a short sway or blink every 5–10 s.
 * Times are milliseconds on any steady clock. UI thread.
 */
class AiDotModel {
public:
  static constexpr float kSizeDp = 56.f;
  /** Gap to the screen edge at rest. */
  static constexpr float kEdgeGapDp = 8.f;
  /** Kept clear under the status bar: the 44dp navigation bar. */
  static constexpr float kTopReserveDp = 44.f;
  /** Kept clear above the home indicator: the 49dp tab bar (the composer is about as tall) and a gap. */
  static constexpr float kBottomReserveDp = 57.f;
  /** A press that moves less than this is a tap. */
  static constexpr float kTapSlopDp = 10.f;
  static constexpr int64_t kTuckAfterMs = 4500;
  static constexpr int64_t kIdleMinMs = 5000;
  static constexpr int64_t kIdleMaxMs = 10000;
  /** The keyframes' lengths in components.rcss (ai-dot-sway, ai-dot-blink); the class goes a little after. */
  static constexpr int64_t kSwayMs = 1200;
  static constexpr int64_t kBlinkMs = 500;
  static constexpr int64_t kIdleTailMs = 100;

  /** Whether the dot shows at all on this screen. */
  static bool Shown(const AiDotScene& scene) {
    return scene.enabled && scene.phone && !scene.call && !scene.pin_gate && !scene.cover && !scene.secret_page &&
           !scene.scanning && !scene.layer_open && !scene.keyboard_up;
  }

  /** The highest and lowest top edge the dot rests at: under the navigation bar, above the tab bar. */
  static float MinTop(const AiDotArea& area) { return area.safe_top_dp + kTopReserveDp; }
  static float MaxTop(const AiDotArea& area) {
    return std::max(MinTop(area), area.height_dp - area.safe_bottom_dp - kBottomReserveDp - kSizeDp);
  }

  /** The dot's top-left corner at rest (tucked: half of it past the edge). */
  static AiDotPoint Rest(const AiDotPlace& place, const AiDotArea& area, const bool tucked) {
    const float half = kSizeDp * 0.5f;
    const float x = place.left ? (tucked ? -half : kEdgeGapDp)
                               : (tucked ? area.width_dp - half : area.width_dp - kSizeDp - kEdgeGapDp);
    const float permille = static_cast<float>(std::clamp(place.y_permille, 0, 1000));
    return {x, MinTop(area) + (MaxTop(area) - MinTop(area)) * permille / 1000.f};
  }

  /** Where a dot let go with its top-left corner at `at` comes to rest: the nearer side, the height kept in the band. */
  static AiDotPlace Snap(const AiDotPoint& at, const AiDotArea& area) {
    AiDotPlace place;
    place.left = at.x_dp + kSizeDp * 0.5f < area.width_dp * 0.5f;
    const float span = MaxTop(area) - MinTop(area);
    const float y = std::clamp(at.y_dp, MinTop(area), MaxTop(area));
    place.y_permille = span > 0.f ? static_cast<int>(std::lround((y - MinTop(area)) * 1000.f / span)) : 0;
    return place;
  }

  /** The pause before idle move number `n` (5–10 s, varied but repeatable). */
  static int64_t IdleDelayMs(const int n) {
    const uint32_t mixed = static_cast<uint32_t>(n + 1) * 2654435761u;
    return kIdleMinMs + static_cast<int64_t>((mixed >> 8) % static_cast<uint32_t>(kIdleMaxMs - kIdleMinMs + 1));
  }

  /** Idle move number `n`: sway and blink take turns; a look without eyes only sways. */
  static AiDotIdle IdleAction(const int n, const bool has_eyes) {
    return has_eyes && n % 2 == 1 ? AiDotIdle::Blink : AiDotIdle::Sway;
  }

  static int64_t IdleLengthMs(const AiDotIdle idle) {
    return idle == AiDotIdle::Blink ? kBlinkMs : idle == AiDotIdle::Sway ? kSwayMs : 0;
  }

  // --- Showing, tucking and idle moves -----------------------------------------------------------------------------

  /** The dot came on screen: out at its edge, the tuck and idle clocks start now. */
  void Show(const int64_t now_ms) {
    shown_ = true;
    tucked_ = false;
    pressed_ = false;
    dragging_ = false;
    idle_ = AiDotIdle::None;
    tuck_at_ms_ = now_ms + kTuckAfterMs;
    next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
  }

  /** The dot went off screen: no clock runs (nothing wakes the app for it). */
  void Hide() {
    shown_ = false;
    pressed_ = false;
    dragging_ = false;
    idle_ = AiDotIdle::None;
    tuck_at_ms_.reset();
    next_idle_ms_.reset();
  }

  struct Changes {
    bool tucked = false; ///< Tucked() flipped.
    bool idle = false;   ///< Idle() changed.
  };

  /** Moves the clocks on to `now_ms`. */
  Changes Advance(const int64_t now_ms, const bool has_eyes) {
    Changes changes;
    if (!shown_) {
      return changes;
    }
    if (!pressed_ && tuck_at_ms_ && now_ms >= *tuck_at_ms_) {
      tuck_at_ms_.reset();
      if (!tucked_) {
        tucked_ = true;
        changes.tucked = true;
      }
    }
    if (idle_ != AiDotIdle::None && now_ms >= idle_end_ms_) {
      idle_ = AiDotIdle::None;
      changes.idle = true;
      ++idle_count_;
      next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
    } else if (idle_ == AiDotIdle::None && !pressed_ && next_idle_ms_ && now_ms >= *next_idle_ms_) {
      idle_ = IdleAction(idle_count_, has_eyes);
      idle_end_ms_ = now_ms + IdleLengthMs(idle_) + kIdleTailMs;
      next_idle_ms_.reset();
      changes.idle = true;
    }
    return changes;
  }

  /** Milliseconds until Advance has something to do (nullopt: nothing, the app may sleep). */
  std::optional<int64_t> MsUntilNext(const int64_t now_ms) const {
    if (!shown_ || pressed_) {
      return std::nullopt;
    }
    std::optional<int64_t> at;
    const auto earliest = [&at](const int64_t t) { at = at ? std::min(*at, t) : t; };
    if (tuck_at_ms_) {
      earliest(*tuck_at_ms_);
    }
    if (idle_ != AiDotIdle::None) {
      earliest(idle_end_ms_);
    } else if (next_idle_ms_) {
      earliest(*next_idle_ms_);
    }
    if (!at) {
      return std::nullopt;
    }
    return std::max<int64_t>(0, *at - now_ms);
  }

  bool ShownNow() const { return shown_; }
  bool Tucked() const { return tucked_; }
  AiDotIdle Idle() const { return idle_; }
  bool Dragging() const { return dragging_; }

  // --- Touch -------------------------------------------------------------------------------------------------------

  /**
   * A finger went down on the dot standing at `rest` (its untucked rest point): it comes out of its tuck, stops its
   * idle move, and no clock runs while it is held.
   */
  void Press(const AiDotPoint& finger_dp, const AiDotPoint& rest) {
    pressed_ = true;
    dragging_ = false;
    tucked_ = false;
    idle_ = AiDotIdle::None;
    tuck_at_ms_.reset();
    press_finger_ = finger_dp;
    press_rest_ = rest;
    offset_ = {};
  }

  /**
   * The finger moved: the offset to draw the dot at from its rest point, or nullopt while it is still a tap. The dot
   * stays on the screen while it is dragged.
   */
  std::optional<AiDotPoint> Move(const AiDotPoint& finger_dp, const AiDotArea& area) {
    if (!pressed_) {
      return std::nullopt;
    }
    const float dx = finger_dp.x_dp - press_finger_.x_dp;
    const float dy = finger_dp.y_dp - press_finger_.y_dp;
    if (!dragging_) {
      if (std::sqrt(dx * dx + dy * dy) <= kTapSlopDp) {
        return std::nullopt;
      }
      dragging_ = true;
    }
    const float x = std::clamp(press_rest_.x_dp + dx, 0.f, std::max(0.f, area.width_dp - kSizeDp));
    const float y = std::clamp(press_rest_.y_dp + dy, area.safe_top_dp,
                               std::max(area.safe_top_dp, area.height_dp - area.safe_bottom_dp - kSizeDp));
    offset_ = {x - press_rest_.x_dp, y - press_rest_.y_dp};
    return offset_;
  }

  struct Release {
    bool tap = false;          ///< No drag: open the ask-AI panel.
    bool moved = false;        ///< Dragged: `place` is where it now rests.
    AiDotPlace place;
    AiDotPoint dropped;        ///< Where the finger left it (top-left), for the slide to `place`.
  };

  /** The finger lifted. The tuck clock starts again. */
  Release Up(const AiDotArea& area, const int64_t now_ms) {
    Release release;
    if (!pressed_) {
      return release;
    }
    pressed_ = false;
    tuck_at_ms_ = now_ms + kTuckAfterMs;
    if (!next_idle_ms_) {
      next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
    }
    if (!dragging_) {
      release.tap = true;
      return release;
    }
    dragging_ = false;
    release.moved = true;
    release.dropped = {press_rest_.x_dp + offset_.x_dp, press_rest_.y_dp + offset_.y_dp};
    release.place = Snap(release.dropped, area);
    return release;
  }

  /** A press that ended without a release (the dot was hidden under the finger). */
  void Cancel(const int64_t now_ms) {
    if (!pressed_) {
      return;
    }
    pressed_ = false;
    dragging_ = false;
    tuck_at_ms_ = now_ms + kTuckAfterMs;
  }

private:
  bool shown_ = false;
  bool tucked_ = false;
  bool pressed_ = false;
  bool dragging_ = false;
  AiDotIdle idle_ = AiDotIdle::None;
  int idle_count_ = 0;
  int64_t idle_end_ms_ = 0;
  std::optional<int64_t> tuck_at_ms_;
  std::optional<int64_t> next_idle_ms_;
  AiDotPoint press_finger_;
  AiDotPoint press_rest_;
  AiDotPoint offset_;
};

} // namespace pbr
