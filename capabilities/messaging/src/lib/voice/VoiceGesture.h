#pragma once

#include <algorithm>

namespace pbr {

/** Numbers of the phone's hold-to-talk bubble, all in dp (the composer converts pointer pixels first). */
namespace voice_dp {
/** Slide up this far from the press point to arm the cancel; slide back under `kDisarm` to take it back. */
inline constexpr float kCancelArm = 56.f;
inline constexpr float kCancelDisarm = 48.f;
inline constexpr float kBubbleWidth = 176.f;
inline constexpr float kBubbleHeight = 64.f;
/** The anchor the bubble aims at only follows the finger once it is this far from it. */
inline constexpr float kAnchorDeadZone = 24.f;
/** The bubble keeps this far from both screen sides. */
inline constexpr float kScreenMargin = 12.f;
/** The bubble's bottom edge is this far above the press point: cancel line + finger radius + spare. */
inline constexpr float kBubbleLift = kCancelArm + 30.f + 12.f;
} // namespace voice_dp

/** The one follow formula: the bubble's left edge for an anchor `a` on a screen `width` wide. */
inline float VoiceBubbleLeft(const float anchor, const float width) {
  const float high = std::max(voice_dp::kScreenMargin, width - voice_dp::kBubbleWidth - voice_dp::kScreenMargin);
  return std::clamp(anchor - voice_dp::kBubbleWidth / 2.f, voice_dp::kScreenMargin, high);
}

/**
 * What a press on the microphone button means, with no UI in it. Phones: hold to record, release
 * sends, sliding up and releasing cancels. Desktop: a click starts, then Send / Cancel (Esc) end it.
 * A press that ends in a way the finger did not choose (a failure) is swallowed to its release. When
 * the recording hits its limit under a held finger the gesture waits for the release (`LimitHold`):
 * releasing sends, sliding up and releasing cancels. A gesture that is lost (no release arrives, the
 * layout changes under the finger) cancels: it never sends.
 *
 * Coordinates are dp; `x` grows to the right, `y` downward.
 */
class VoiceGesture {
public:
  enum class Mode { Hold, Click };
  enum class Phase { Idle, Recording, Swallowing, LimitHold };
  enum class Action { None, Start, Send, Cancel };

  explicit VoiceGesture(const Mode mode = Mode::Hold, const float cancel_distance = voice_dp::kCancelArm)
      : mode_(mode), cancel_distance_(cancel_distance) {}

  /** The button pressed (Hold) or clicked (Click). */
  Action Press(const float x, const float y) {
    if (phase_ != Phase::Idle) {
      return Action::None;
    }
    phase_ = Phase::Recording;
    press_y_ = y;
    anchor_x_ = x;
    in_cancel_zone_ = false;
    return Action::Start;
  }

  /**
   * The pointer moved while down. Far enough up arms the cancel (and 8dp less keeps it armed); sideways moves
   * only drag the bubble's anchor along, once the finger is past the dead zone, and never touch the cancel.
   */
  void Move(const float x, const float y) {
    if (mode_ != Mode::Hold || (phase_ != Phase::Recording && phase_ != Phase::LimitHold)) {
      return;
    }
    anchor_x_ = std::clamp(anchor_x_, x - voice_dp::kAnchorDeadZone, x + voice_dp::kAnchorDeadZone);
    const float up = press_y_ - y;
    in_cancel_zone_ = in_cancel_zone_ ? up >= cancel_distance_ - (voice_dp::kCancelArm - voice_dp::kCancelDisarm)
                                      : up >= cancel_distance_;
  }

  /** The finger / button went up. */
  Action Release() {
    if (mode_ != Mode::Hold) {
      return Action::None;
    }
    if (phase_ == Phase::Swallowing) {
      phase_ = Phase::Idle;
      return Action::None;
    }
    if (phase_ != Phase::Recording && phase_ != Phase::LimitHold) {
      return Action::None;
    }
    phase_ = Phase::Idle;
    return in_cancel_zone_ ? Action::Cancel : Action::Send;
  }

  /** No release will come, or the ground moved: cancel, never send. */
  Action Lost() {
    const bool recording = phase_ == Phase::Recording || phase_ == Phase::LimitHold;
    phase_ = Phase::Idle;
    in_cancel_zone_ = false;
    return recording ? Action::Cancel : Action::None;
  }

  /** The Send button (Click mode). */
  Action SendNow() {
    if (phase_ != Phase::Recording) {
      return Action::None;
    }
    phase_ = Phase::Idle;
    return Action::Send;
  }

  /** The Cancel button or Esc. */
  Action CancelNow() {
    if (phase_ != Phase::Recording) {
      return Action::None;
    }
    phase_ = Phase::Idle;
    in_cancel_zone_ = false;
    return Action::Cancel;
  }

  /**
   * The recording hit its limit. A held finger keeps the gesture alive (`LimitHold`: the release decides);
   * anything else is over.
   */
  void RecorderLimit(const bool pointer_down) {
    if (phase_ != Phase::Recording) {
      return;
    }
    if (mode_ == Mode::Hold && pointer_down) {
      phase_ = Phase::LimitHold;
    } else {
      phase_ = Phase::Idle;
      in_cancel_zone_ = false;
    }
  }

  /**
   * The recording ended by itself in a way the finger did not choose (a failure): a held press is swallowed
   * until it is released.
   */
  void RecorderEnded(const bool pointer_down) {
    if (phase_ != Phase::Recording) {
      return;
    }
    phase_ = (mode_ == Mode::Hold && pointer_down) ? Phase::Swallowing : Phase::Idle;
    in_cancel_zone_ = false;
  }

  Phase phase() const { return phase_; }
  bool Recording() const { return phase_ == Phase::Recording; }
  /** The recorder stopped at its limit and the finger is still down. */
  bool AtLimit() const { return phase_ == Phase::LimitHold; }
  bool InCancelZone() const { return in_cancel_zone_; }
  /** Where the bubble aims, in dp. */
  float anchor_x() const { return anchor_x_; }
  Mode mode() const { return mode_; }

private:
  Mode mode_;
  float cancel_distance_;
  Phase phase_ = Phase::Idle;
  float press_y_ = 0.f;
  float anchor_x_ = 0.f;
  bool in_cancel_zone_ = false;
};

} // namespace pbr
