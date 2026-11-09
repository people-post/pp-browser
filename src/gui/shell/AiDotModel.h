#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

namespace pbr {

/** What is on screen, for deciding whether the phone AI dot shows (DESIGN-pp-ui §8.14, PLAN-ui-batch5-ask-ai 5c). */
struct AiDotScene {
  bool enabled = true;     ///< Me → AI assistant → Show AI dot.
  bool phone = true;       ///< Compact layout; desktop has no floating dot in this batch.
  bool call = false;       ///< An incoming ring or the call screen; a call minimised to its bar is not one.
  bool pin_gate = false;   ///< The PIN gate (unlock, create, the first-run identity fork).
  bool cover = false;      ///< The startup cover or the onboarding wizard.
  bool secret_page = false;///< Me → Account & security: change PIN, transfer, the recovery phrase.
  bool scanning = false;   ///< The full-screen QR scanner.
  bool layer_open = false; ///< Any layer over the page: a page sheet (the ask-AI panel too) or a panel.
  bool keyboard_up = false;///< The on-screen keyboard or the emoji panel.
};

/**
 * A short action the dot plays (RCSS keyframes switched on by a class; nothing runs between them). The motions and
 * their particles are the mockups' (DEV/ui-mockups/mascot-v1.html, mascot-v2.html, mascot-compare.html):
 * - Sway: rocks side to side. Blink. Look: the two bubbles glance at each other (the mockup's idle glance).
 * - Talk: the mouth opens and shuts (C: three equalizer bars; D: text lines write themselves), the partner nods.
 * - Shy: the front bubble leans away and peeks aside with its eyes half shut and pink cheeks, the partner ducks behind.
 * - Wave: the front bubble waves its body with happy eyes, the partner hops twice.
 * - Listen: leans in with big eyes, both nod, signal arcs pulse beside the partner (D: equalizer bars in its bubble).
 * - Think: eyes up and right, a "typing" bubble with three bouncing dots (D: the dots in its bubble).
 * - Five: the two bump into a high five with happy eyes and a star (C, D: sparkle lines).
 * - Happy: both jump one after the other with happy eyes, cheeks and two floating hearts (C, D: a nod; D draws a
 *   check).
 * - Private: closes its eyes, turns away and shows a lock ("I don't read your chats").
 * - Float: the mockups' float, drifting gently up and down once for 3.25 s and back at rest (C: floatC, half as far);
 *   it was a continuous loop there, here it is one timed action (Kenneth 2026-11-04) so nothing runs between them.
 * Not in the random rotation: Sleep (closed eyes, a breath, three rising z's; once after kSleepAfterMs without a touch,
 * then it stays still, asleep, until the next touch), Poke (a tap: it squashes and wobbles, then the panel opens) and
 * Pop (a tap on it tucked: it jumps out, the mockups' squash-and-stretch jump, then the panel opens).
 */
enum class AiDotIdle {
  None, Sway, Blink, Look, Talk, Shy, Wave, Listen, Think, Five, Happy, Private, Sleep, Poke, Pop, Float
};

/** The assistant look (Me → AI assistant): `a` one eye, `b` two eyes (default), `c` cool, `d` plain (no face), and the
 * weekend looks `e` Friday (sunglasses, a lazy sway), `f` Saturday (cap, the liveliest), `g` Sunday (nightcap, half-open
 * eyes, a mug; slow). */
enum class AiDotLook { OneEye, TwoEyes, Cool, Plain, Evening, Outing, Lazy };

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
 * dragged anywhere, on release it sticks to the nearer left or right edge inside the safe area. It never tucks on a
 * timer (Kenneth 2026-11-04): the user tucks it half off the screen by letting go with more than half of it past the
 * edge, or by flinging it fast toward an edge; a tap on a tucked dot makes it jump out and then opens the panel, and
 * it stays out (原话 21.2: "点它的时候他就自己跳出来"); a drag brings it out too. While it is out, every 3–6 s it
 * plays one idle action picked at random, never the same one twice in a row (Kenneth 2026-11-04: "说话呀、害羞啊……
 * 都没有看到"; 原话 21: all the mockup's motions, hearts, signal arcs and stars); tucked, it does nothing, so nothing
 * wakes the app. Left alone for kSleepAfterMs it plays Sleep once and then rests asleep with nothing scheduled; any
 * touch wakes it. A tap on it out plays Poke and opens the panel when the poke has played (Changes::open).
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
  /** Let go with its middle within this share of its size from a side edge (or past it) counts as "more than half
   * past": a finger cannot go beyond the screen, so a dot grabbed anywhere and pushed into the edge must tuck. */
  static constexpr float kEdgeSlack = 0.25f;
  /** A horizontal fling toward an edge at least this fast tucks the dot (the sheet model's flick speed)... */
  static constexpr float kFlingDpPerS = 700.f;
  /** ...once the finger has moved this far (a wobble is no fling). */
  static constexpr float kFlingMinDragDp = 24.f;
  /** Moves older than this at release do not count towards the fling speed (a drag that stopped is no fling). */
  static constexpr int64_t kFlingWindowMs = 100;
  static constexpr int64_t kIdleMinMs = 3000;
  static constexpr int64_t kIdleMaxMs = 6000;
  /** No touch anywhere in the app for this long: the next action is Sleep, and after it nothing until a touch. */
  static constexpr int64_t kSleepAfterMs = 120000;
  /** The keyframes' lengths in components.rcss (ai-dot-sway, ai-dot-blink, …); the class goes a little after. The calm
   * looks (C, D) wave and poke more slowly (the mockup's tiltC / hopC 2.2 s, pokeC 0.45 s). */
  static constexpr int64_t kSwayMs = 1560;
  static constexpr int64_t kBlinkMs = 650;
  static constexpr int64_t kLookMs = 2600;
  static constexpr int64_t kTalkMs = 2184;
  static constexpr int64_t kShyMs = 2080;
  static constexpr int64_t kWaveMs = 2080;
  static constexpr int64_t kWaveCalmMs = 2860;
  static constexpr int64_t kListenMs = 3120;
  static constexpr int64_t kThinkMs = 3120;
  static constexpr int64_t kFiveMs = 1950;
  static constexpr int64_t kHappyMs = 2340;
  static constexpr int64_t kPrivateMs = 2600;
  static constexpr int64_t kSleepMs = 5200;
  static constexpr int64_t kPokeMs = 780;
  static constexpr int64_t kPokeCalmMs = 585;
  static constexpr int64_t kPopMs = 910;
  static constexpr int64_t kFloatMs = 3250;
  /** The lazy look (G, Sunday) is slower by design (the weekend mockup's 1.3 x longer timings): these actions last
   * longer than the shared ones above; the others are shared. */
  static constexpr int64_t kTalkSlowMs = 3120; ///< The yawn.
  static constexpr int64_t kWaveSlowMs = 2600;
  static constexpr int64_t kFiveSlowMs = 2860;
  static constexpr int64_t kHappySlowMs = 3120;
  static constexpr int64_t kPokeSlowMs = 1040;
  static constexpr int64_t kFloatSlowMs = 5200; ///< One slow breath.
  static constexpr int64_t kIdleTailMs = 100;

  static AiDotLook LookFrom(const std::string_view look) {
    return look == "a"   ? AiDotLook::OneEye
           : look == "c" ? AiDotLook::Cool
           : look == "d" ? AiDotLook::Plain
           : look == "e" ? AiDotLook::Evening
           : look == "f" ? AiDotLook::Outing
           : look == "g" ? AiDotLook::Lazy
                         : AiDotLook::TwoEyes;
  }

  /** C and D move less and more slowly (DESIGN-pp-ui §8.14). */
  static bool Calm(const AiDotLook look) { return look == AiDotLook::Cool || look == AiDotLook::Plain; }

  /** G moves more slowly: longer talk (a yawn), wave, five, happy, poke and float. */
  static bool Slow(const AiDotLook look) { return look == AiDotLook::Lazy; }

  /** The random rotation of a look: every mockup action but Sleep and Poke; the plain look has no eyes, so no blink and
   * no glance (its talk, listen, think, happy and private show symbols in its bubble instead). */
  static std::span<const AiDotIdle> IdleActions(const AiDotLook look) {
    static constexpr AiDotIdle kFace[] = {AiDotIdle::Sway,   AiDotIdle::Blink, AiDotIdle::Look,  AiDotIdle::Talk,
                                          AiDotIdle::Shy,    AiDotIdle::Wave,  AiDotIdle::Listen, AiDotIdle::Think,
                                          AiDotIdle::Five,   AiDotIdle::Happy, AiDotIdle::Private, AiDotIdle::Float};
    static constexpr AiDotIdle kPlain[] = {AiDotIdle::Sway,  AiDotIdle::Talk,  AiDotIdle::Shy,
                                           AiDotIdle::Wave,  AiDotIdle::Listen, AiDotIdle::Think,
                                           AiDotIdle::Five,  AiDotIdle::Happy, AiDotIdle::Private, AiDotIdle::Float};
    return look == AiDotLook::Plain ? std::span<const AiDotIdle>(kPlain) : std::span<const AiDotIdle>(kFace);
  }

  /** One of the look's actions other than `previous`, chosen by `roll` (any random number). */
  static AiDotIdle PickIdle(const AiDotLook look, const AiDotIdle previous, const uint32_t roll) {
    AiDotIdle choices[16];
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
    case AiDotIdle::Look:
      return "look";
    case AiDotIdle::Talk:
      return "talk";
    case AiDotIdle::Shy:
      return "shy";
    case AiDotIdle::Wave:
      return "wave";
    case AiDotIdle::Listen:
      return "listen";
    case AiDotIdle::Think:
      return "think";
    case AiDotIdle::Five:
      return "five";
    case AiDotIdle::Happy:
      return "happy";
    case AiDotIdle::Private:
      return "private";
    case AiDotIdle::Sleep:
      return "sleep";
    case AiDotIdle::Poke:
      return "poke";
    case AiDotIdle::Pop:
      return "pop";
    case AiDotIdle::Float:
      return "float";
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

  /** The pause before idle move number `n` (3–6 s, varied but repeatable). */
  static int64_t IdleDelayMs(const int n) {
    const uint32_t mixed = static_cast<uint32_t>(n + 1) * 2654435761u;
    return kIdleMinMs + static_cast<int64_t>((mixed >> 8) % static_cast<uint32_t>(kIdleMaxMs - kIdleMinMs + 1));
  }

  static int64_t IdleLengthMs(const AiDotIdle idle, const AiDotLook look) {
    switch (idle) {
    case AiDotIdle::Sway:
      return kSwayMs;
    case AiDotIdle::Blink:
      return kBlinkMs;
    case AiDotIdle::Look:
      return kLookMs;
    case AiDotIdle::Talk:
      return Slow(look) ? kTalkSlowMs : kTalkMs;
    case AiDotIdle::Shy:
      return kShyMs;
    case AiDotIdle::Wave:
      return Calm(look) ? kWaveCalmMs : Slow(look) ? kWaveSlowMs : kWaveMs;
    case AiDotIdle::Listen:
      return kListenMs;
    case AiDotIdle::Think:
      return kThinkMs;
    case AiDotIdle::Five:
      return Slow(look) ? kFiveSlowMs : kFiveMs;
    case AiDotIdle::Happy:
      return Slow(look) ? kHappySlowMs : kHappyMs;
    case AiDotIdle::Private:
      return kPrivateMs;
    case AiDotIdle::Sleep:
      return kSleepMs;
    case AiDotIdle::Poke:
      return Calm(look) ? kPokeCalmMs : Slow(look) ? kPokeSlowMs : kPokeMs;
    case AiDotIdle::Pop:
      return kPopMs;
    case AiDotIdle::Float:
      return Slow(look) ? kFloatSlowMs : kFloatMs;
    case AiDotIdle::None:
      break;
    }
    return 0;
  }

  // --- Showing, tucking and idle moves -----------------------------------------------------------------------------

  /** The dot came on screen: at its edge, out or `tucked` (as the user left it); the idle clock starts now. */
  void Show(const int64_t now_ms, const bool tucked = false) {
    shown_ = true;
    tucked_ = tucked;
    pressed_ = false;
    dragging_ = false;
    asleep_ = false;
    open_when_done_ = false;
    last_touch_ms_ = now_ms;
    idle_ = AiDotIdle::None;
    next_idle_ms_ = tucked_ ? std::nullopt : std::optional<int64_t>(now_ms + IdleDelayMs(idle_count_));
  }

  /** The dot went off screen: no clock runs (nothing wakes the app for it); a poke's panel no longer opens. */
  void Hide() {
    shown_ = false;
    pressed_ = false;
    dragging_ = false;
    asleep_ = false;
    open_when_done_ = false;
    idle_ = AiDotIdle::None;
    next_idle_ms_.reset();
  }

  /**
   * A touch or a key anywhere in the app: the sleep clock starts again, and an asleep dot (or one falling asleep)
   * wakes, with its next action scheduled. True when that changed Asleep() or Idle().
   */
  bool Touch(const int64_t now_ms) {
    last_touch_ms_ = now_ms;
    if (!asleep_ && idle_ != AiDotIdle::Sleep) {
      return false;
    }
    asleep_ = false;
    if (idle_ == AiDotIdle::Sleep) {
      idle_ = AiDotIdle::None;
      ++idle_count_;
    }
    if (shown_ && !pressed_) {
      ResumeIdle(now_ms);
    }
    return true;
  }

  struct Changes {
    bool idle = false; ///< Idle() or Asleep() changed.
    bool open = false; ///< A tap's poke has played: open the ask-AI panel now.
  };

  /** Moves the clocks on to `now_ms`; `look` decides which actions may play and how long they last. */
  Changes Advance(const int64_t now_ms, const AiDotLook look) {
    look_ = look;
    Changes changes;
    if (!shown_) {
      return changes;
    }
    if (tucked_) {
      next_idle_ms_.reset(); // tucked away it rests; a drag brings it out and starts the actions again
    }
    if (idle_ != AiDotIdle::None && now_ms >= idle_end_ms_) {
      const AiDotIdle ended = idle_;
      idle_ = AiDotIdle::None;
      changes.idle = true;
      ++idle_count_;
      if (ended == AiDotIdle::Sleep) {
        // It rests asleep (Asleep() since the sleep began, so the still pose takes over its end): nothing until a
        // touch.
      } else if (!tucked_) {
        next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
      }
      if (open_when_done_) {
        open_when_done_ = false;
        changes.open = true;
      }
    } else if (idle_ == AiDotIdle::None && !pressed_ && !tucked_ && !asleep_ && next_idle_ms_ &&
               now_ms >= *next_idle_ms_) {
      if (now_ms - last_touch_ms_ >= kSleepAfterMs) {
        idle_ = AiDotIdle::Sleep;
        asleep_ = true;
      } else {
        idle_ = PickIdle(look, last_idle_, static_cast<uint32_t>(rng_()));
        last_idle_ = idle_;
      }
      idle_end_ms_ = now_ms + IdleLengthMs(idle_, look) + kIdleTailMs;
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
  /** It is falling asleep or asleep: eyes shut, and after Sleep has played nothing is scheduled until a touch. */
  bool Asleep() const { return asleep_; }
  bool Dragging() const { return dragging_; }

  // --- Touch -------------------------------------------------------------------------------------------------------

  /**
   * A finger went down on the dot standing at `rest` (where it is drawn now, tucked or not): its idle move stops (a
   * poke still playing too, and its panel no longer opens: this press decides again) and no clock runs while it is
   * held. A tucked dot stays tucked until it is dragged.
   */
  void Press(const AiDotPoint& finger_dp, const AiDotPoint& rest) {
    pressed_ = true;
    dragging_ = false;
    open_when_done_ = false;
    asleep_ = false; // touched: awake
    idle_ = AiDotIdle::None;
    samples_.clear();
    press_finger_ = finger_dp;
    press_rest_ = rest;
    offset_ = {};
  }

  /**
   * The finger moved: the offset to draw the dot at from its rest point, or nullopt while it is still a tap. The dot
   * may go up to three quarters past a side edge (to be tucked there), never past the top or bottom; dragging a
   * tucked dot brings it out.
   */
  std::optional<AiDotPoint> Move(const AiDotPoint& finger_dp, const AiDotArea& area, const int64_t now_ms) {
    if (!pressed_) {
      return std::nullopt;
    }
    samples_.push_back({now_ms, finger_dp.x_dp});
    if (samples_.size() > 16) {
      samples_.erase(samples_.begin());
    }
    const float dx = finger_dp.x_dp - press_finger_.x_dp;
    const float dy = finger_dp.y_dp - press_finger_.y_dp;
    if (!dragging_) {
      if (std::sqrt(dx * dx + dy * dy) <= kTapSlopDp) {
        return std::nullopt;
      }
      dragging_ = true;
      tucked_ = false;
    }
    const float past = area.size_dp * 0.75f;
    const float x = std::clamp(press_rest_.x_dp + dx, -past, std::max(-past, area.width_dp - area.size_dp + past));
    const float y = std::clamp(press_rest_.y_dp + dy, area.safe_top_dp,
                               std::max(area.safe_top_dp, area.height_dp - area.safe_bottom_dp - area.size_dp));
    offset_ = {x - press_rest_.x_dp, y - press_rest_.y_dp};
    return offset_;
  }

  struct Release {
    bool tap = false;          ///< No drag: a poke, or a jump out of the tuck; then Changes::open opens the panel.
    bool untucked = false;     ///< The tap was on it tucked: it is out now, at the same place (persist that).
    bool moved = false;        ///< Dragged: `place` is where it now rests.
    AiDotPlace place;
    bool tucked = false;       ///< Dragged past the edge or flung at it: `place` is its side, half off the screen.
    AiDotPoint dropped;        ///< Where the finger left it (top-left), for the slide to `place`.
  };

  /** The finger lifted: a tap, or a drag that rests at an edge, out or tucked (see the class comment). */
  Release Up(const AiDotArea& area, const int64_t now_ms) {
    Release release;
    if (!pressed_) {
      return release;
    }
    pressed_ = false;
    last_touch_ms_ = now_ms;
    if (!dragging_) {
      release.tap = true;
      // The poke (out) or the jump out of the tuck plays first (RCSS keyframes), then the panel opens; nothing else
      // is scheduled meanwhile. Out of its tuck it stays out, also after the panel.
      release.untucked = tucked_;
      tucked_ = false;
      idle_ = release.untucked ? AiDotIdle::Pop : AiDotIdle::Poke;
      idle_end_ms_ = now_ms + IdleLengthMs(idle_, look_) + kIdleTailMs;
      next_idle_ms_.reset();
      open_when_done_ = true;
      return release;
    }
    dragging_ = false;
    release.moved = true;
    release.dropped = {press_rest_.x_dp + offset_.x_dp, press_rest_.y_dp + offset_.y_dp};
    release.place = Snap(release.dropped, area);
    const float vx = FlingDpPerS(now_ms);
    const float dx = release.dropped.x_dp - press_rest_.x_dp;
    const float half = area.size_dp * 0.5f;
    const bool past_left = release.dropped.x_dp <= -half + area.size_dp * kEdgeSlack;
    const bool past_right = release.dropped.x_dp >= area.width_dp - half - area.size_dp * kEdgeSlack;
    const bool fling = std::fabs(vx) >= kFlingDpPerS && std::fabs(dx) >= kFlingMinDragDp;
    if (past_left || past_right) {
      release.tucked = true;
      release.place.left = past_left;
    } else if (fling) {
      release.tucked = true;
      release.place.left = vx < 0.f;
    }
    tucked_ = release.tucked;
    ResumeIdle(now_ms);
    return release;
  }

  /** A press that ended without a release (the dot was hidden under the finger). */
  void Cancel(const int64_t now_ms) {
    if (!pressed_) {
      return;
    }
    pressed_ = false;
    dragging_ = false;
    ResumeIdle(now_ms);
  }

private:
  struct Sample {
    int64_t at_ms = 0;
    float x_dp = 0.f;
  };

  /** Starts the idle clock if none runs and the dot is out; tucked, none runs. */
  void ResumeIdle(const int64_t now_ms) {
    if (tucked_) {
      next_idle_ms_.reset();
    } else if (!next_idle_ms_) {
      next_idle_ms_ = now_ms + IdleDelayMs(idle_count_);
    }
  }

  /** The finger's horizontal speed over the last kFlingWindowMs before release (0: it had stopped). */
  float FlingDpPerS(const int64_t now_ms) const {
    const Sample* first = nullptr;
    const Sample* last = nullptr;
    for (const Sample& sample : samples_) {
      if (now_ms - sample.at_ms > kFlingWindowMs) {
        continue;
      }
      first = first ? first : &sample;
      last = &sample;
    }
    if (!first || last->at_ms <= first->at_ms) {
      return 0.f;
    }
    return (last->x_dp - first->x_dp) * 1000.f / static_cast<float>(last->at_ms - first->at_ms);
  }

  bool shown_ = false;
  bool tucked_ = false;
  bool asleep_ = false;
  /** A tap's poke is playing: open the panel when it ends. */
  bool open_when_done_ = false;
  /** The look last passed to Advance (a tap's poke lasts as long as that look's). */
  AiDotLook look_ = AiDotLook::TwoEyes;
  int64_t last_touch_ms_ = 0;
  bool pressed_ = false;
  bool dragging_ = false;
  AiDotIdle idle_ = AiDotIdle::None;
  /** The action played last (it is not picked again next). */
  AiDotIdle last_idle_ = AiDotIdle::None;
  std::minstd_rand rng_{1u};
  int idle_count_ = 0;
  int64_t idle_end_ms_ = 0;
  std::vector<Sample> samples_;
  std::optional<int64_t> next_idle_ms_;
  AiDotPoint press_finger_;
  AiDotPoint press_rest_;
  AiDotPoint offset_;
};

} // namespace pbr
