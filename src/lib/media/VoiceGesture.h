#pragma once

namespace pbr {

/**
 * What a press on the microphone button means, with no UI in it. Phones: hold to record, release
 * sends, sliding up and releasing cancels. Desktop: a click starts, then Send / Cancel (Esc) end it.
 * A press that ends in a way the finger did not choose (the recording hit its limit, a call came)
 * is swallowed to its release, so the lift does not send a second time. A gesture that is lost (no
 * release arrives, the layout changes under the finger) cancels: it never sends.
 */
class VoiceGesture {
public:
  enum class Mode { Hold, Click };
  enum class Phase { Idle, Recording, Swallowing };
  enum class Action { None, Start, Send, Cancel };

  explicit VoiceGesture(const Mode mode = Mode::Hold, const float cancel_distance = 56.f)
      : mode_(mode), cancel_distance_(cancel_distance) {}

  /** The button pressed (Hold) or clicked (Click). `y` grows downward. */
  Action Press(const float y) {
    if (phase_ != Phase::Idle) {
      return Action::None;
    }
    phase_ = Phase::Recording;
    press_y_ = y;
    in_cancel_zone_ = false;
    return Action::Start;
  }

  /** The pointer moved while down: far enough up arms the cancel. */
  void Move(const float y) {
    if (mode_ == Mode::Hold && phase_ == Phase::Recording) {
      in_cancel_zone_ = press_y_ - y >= cancel_distance_;
    }
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
    if (phase_ != Phase::Recording) {
      return Action::None;
    }
    phase_ = Phase::Idle;
    return in_cancel_zone_ ? Action::Cancel : Action::Send;
  }

  /** No release will come, or the ground moved: cancel, never send. */
  Action Lost() {
    const bool recording = phase_ == Phase::Recording;
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
   * The recording ended by itself (limit reached, call came): what it has is being sent. A held press
   * is swallowed until it is released.
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
  bool InCancelZone() const { return in_cancel_zone_; }
  Mode mode() const { return mode_; }

private:
  Mode mode_;
  float cancel_distance_;
  Phase phase_ = Phase::Idle;
  float press_y_ = 0.f;
  bool in_cancel_zone_ = false;
};

} // namespace pbr
