#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>

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

/**
 * The short idle action the dot plays now and then (RCSS keyframes switched on by a class; nothing runs between them).
 * The motions are the mockups' (DEV/ui-mockups/mascot-compare.html): sway, blink, hop (the "happy" jump), talk (the
 * mouth opens and shuts; the plain look shows text lines instead), shy (leans away, eyes aside, cheeks pink), look
 * around (eyes left, then right).
 */
enum class AiDotIdle { None, Sway, Blink, Hop, Talk, Shy, Look };

/** The assistant look (Me → AI assistant): `a` one eye, `b` two eyes (default), `c` cool, `d` plain (no face). */
enum class AiDotLook { OneEye, TwoEyes, Cool, Plain };

/** Where the dot rests: the side it sticks to and how far down the band it stands (0 top, 1000 bottom). */
struct AiDotPlace {
  bool left = false;
  int y_permille = 600;

  bool operator==(const AiDotPlace& other) const { return left == other.left && y_permille == other.y_permille; }
  bool operator!=(const AiDotPlace& other) const { return !(*this == other); }
};

/**
 * The screen the dot lives on, in dp: the whole document body (edge to edge on phones) and its safe areas, and the
 * dot's own side there (Me → AI assistant → Assistant size; AiDotModel::SizeDp).
 */
struct AiDotArea {
  float width_dp = 0.f;
  float height_dp = 0.f;
  float safe_top_dp = 0.f;
  float safe_bottom_dp = 0.f;
  float size_dp = 64.f;
};

struct AiDotPoint {
  float x_dp = 0.f;
  float y_dp = 0.f;
};

/**
 * The phone AI dot without any DOM (Kenneth 2026-11-02/03: "可以到处放，或者把它吸在侧边", "设置里面设置打开或关闭"):
 * dragged anywhere, on release it sticks to the nearer left or right edge inside the safe area; 15 s after the last
 * touch it tucks half off the screen, a press brings it out again. While it is out (not tucked), every 5–10 s it plays
 * one idle action picked at random, never the same one twice in a row (Kenneth 2026-11-04: "说话呀、害羞啊……都没有看到，
 * ……他靠边的这个时间稍微慢一点"); tucked, it does nothing, so nothing wakes the app.
 * Its side is the user's choice (Kenneth 2026-11-04: "三个尺寸小中大"; AiDotArea::size_dp); the tuck, the gap to the
 * edge and the clamps follow it. Times are milliseconds on any steady clock. UI thread.
 */
class AiDotModel {
public:
  /** Me → AI assistant → Assistant size: `s` small, `m` medium (the default; 56dp looked too small), `l` large. */
  static constexpr float kSmallDp = 52.f;
  static constexpr float kMediumDp = 64.f;
  static constexpr float kLargeDp = 76.f;
  static float SizeDp(const std::string_view size) {
    return size == "s" ? kSmallDp : size == "l" ? kLargeDp : kMediumDp;
  }
  /** Gap to the screen edge at rest: 8dp on the first 56dp dot, as much more as the dot is bigger. */
  static float EdgeGapDp(const float size_dp) { return size_dp / 7.f; }
  /** Kept clear under the status bar: the 44dp navigation bar. */
  static constexpr float kTopReserveDp = 44.f;
  /** Kept clear above the home indicator: the 49dp tab bar (the composer is about as tall) and a gap. */
  static constexpr float kBottomReserveDp = 57.f;
  /** A press that moves less than this is a tap. */
  static constexpr float kTapSlopDp = 10.f;
  static constexpr int64_t kTuckAfterMs = 15000;
  static constexpr int64_t kIdleMinMs = 5000;
  static constexpr int64_t kIdleMaxMs = 10000;
  /** The keyframes' lengths in components.rcss (ai-dot-sway, ai-dot-blink, …); the class goes a little after. */
  static constexpr int64_t kSwayMs = 1200;
  static constexpr int64_t kBlinkMs = 500;
  static constexpr int64_t kHopMs = 1300;
  static constexpr int64_t kTalkMs = 1680;
  static constexpr int64_t kShyMs = 1600;
  static constexpr int64_t kLookMs = 1800;
  static constexpr int64_t kIdleTailMs = 100;

  static AiDotLook LookFrom(const std::string_view look) {
    return look == "a" ? AiDotLook::OneEye : look == "c" ? AiDotLook::Cool : look == "d" ? AiDotLook::Plain
                                                                                      : AiDotLook::TwoEyes;
  }

  /** The actions a look plays: the plain look has no eyes, so only hop, sway and talk (its text lines). */
  static std::span<const AiDotIdle> IdleActions(const AiDotLook look) {
    static constexpr AiDotIdle kFace[] = {AiDotIdle::Sway, AiDotIdle::Blink, AiDotIdle::Hop,
                                          AiDotIdle::Talk, AiDotIdle::Shy,   AiDotIdle::Look};
    static constexpr AiDotIdle kPlain[] = {AiDotIdle::Sway, AiDotIdle::Hop, AiDotIdle::Talk};
    return look == AiDotLook::Plain ? std::span<const AiDotIdle>(kPlain) : std::span<const AiDotIdle>(kFace);
  }

  /** One of the look's actions other than `previous`, chosen by `roll` (any random number). */
  static AiDotIdle PickIdle(const AiDotLook look, const AiDotIdle previous, const uint32_t roll) {
    AiDotIdle choices[8];
    uint32_t count = 0;
    for (const AiDotIdle idle : IdleActions(look)) {
      if (idle != previous) {
        choices[count++] = idle;
      }
    }
    return count == 0 ? AiDotIdle::None : choices[roll % count];
  }

  /** The class suffix (ai-dot--<name>) and bound value for an action; empty for none. */
  static const char* IdleName(const AiDotIdle idle) {
    switch (idle) {
    case AiDotIdle::Sway:
      return "sway";
    case AiDotIdle::Blink:
      return "blink";
    case AiDotIdle::Hop:
      return "hop";
    case AiDotIdle::Talk:
      return "talk";
    case AiDotIdle::Shy:
      return "shy";
    case AiDotIdle::Look:
      return "look";
    case AiDotIdle::None:
      break;
    }
    return "";
  }

  /** The random actions; the app seeds it once, tests with a fixed seed. */
  void Seed(const uint32_t seed) { rng_.seed(seed); }

  /** Whether the dot shows at all on this screen. */
  static bool Shown(const AiDotScene& scene) {
    return scene.enabled && scene.phone && !scene.call && !scene.pin_gate && !scene.cover && !scene.secret_page &&
           !scene.scanning && !scene.layer_open && !scene.keyboard_up;
  }

  /** The highest and lowest top edge the dot rests at: under the navigation bar, above the tab bar. */
  static float MinTop(const AiDotArea& area) { return area.safe_top_dp + kTopReserveDp; }
  static float MaxTop(const AiDotArea& area) {
    return std::max(MinTop(area), area.height_dp - area.safe_bottom_dp - kBottomReserveDp - area.size_dp);
  }

  /** The dot's top-left corner at rest (tucked: half of it past the edge). */
  static AiDotPoint Rest(const AiDotPlace& place, const AiDotArea& area, const bool tucked) {
    const float size = area.size_dp;
    const float half = size * 0.5f;
    const float gap = EdgeGapDp(size);
    const float x = place.left ? (tucked ? -half : gap) : (tucked ? area.width_dp - half : area.width_dp - size - gap);
    const float permille = static_cast<float>(std::clamp(place.y_permille, 0, 1000));
    return {x, MinTop(area) + (MaxTop(area) - MinTop(area)) * permille / 1000.f};
  }

  /** Where a dot let go with its top-left corner at `at` comes to rest: the nearer side, the height kept in the band. */
  static AiDotPlace Snap(const AiDotPoint& at, const AiDotArea& area) {
    AiDotPlace place;
    place.left = at.x_dp + area.size_dp * 0.5f < area.width_dp * 0.5f;
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

  static int64_t IdleLengthMs(const AiDotIdle idle) {
    switch (idle) {
    case AiDotIdle::Sway:
      return kSwayMs;
    case AiDotIdle::Blink:
      return kBlinkMs;
    case AiDotIdle::Hop:
      return kHopMs;
    case AiDotIdle::Talk:
      return kTalkMs;
    case AiDotIdle::Shy:
      return kShyMs;
    case AiDotIdle::Look:
      return kLookMs;
    case AiDotIdle::None:
      break;
    }
    return 0;
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

  /** Moves the clocks on to `now_ms`; `look` decides which actions may play. */
  Changes Advance(const int64_t now_ms, const AiDotLook look) {
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
    if (tucked_) {
      next_idle_ms_.reset(); // tucked away it rests; a press brings it out and starts the actions again
    }
    if (idle_ != AiDotIdle::None && now_ms >= idle_end_ms_) {
      idle_ = AiDotIdle::None;
      changes.idle = true;
      ++idle_count_;
      if (!tucked_) {
        next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
      }
    } else if (idle_ == AiDotIdle::None && !pressed_ && !tucked_ && next_idle_ms_ && now_ms >= *next_idle_ms_) {
      idle_ = PickIdle(look, last_idle_, static_cast<uint32_t>(rng_()));
      last_idle_ = idle_;
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
    const float x = std::clamp(press_rest_.x_dp + dx, 0.f, std::max(0.f, area.width_dp - area.size_dp));
    const float y = std::clamp(press_rest_.y_dp + dy, area.safe_top_dp,
                               std::max(area.safe_top_dp, area.height_dp - area.safe_bottom_dp - area.size_dp));
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
  /** The action played last (it is not picked again next). */
  AiDotIdle last_idle_ = AiDotIdle::None;
  std::minstd_rand rng_{1u};
  int idle_count_ = 0;
  int64_t idle_end_ms_ = 0;
  std::optional<int64_t> tuck_at_ms_;
  std::optional<int64_t> next_idle_ms_;
  AiDotPoint press_finger_;
  AiDotPoint press_rest_;
  AiDotPoint offset_;
};

} // namespace pbr
