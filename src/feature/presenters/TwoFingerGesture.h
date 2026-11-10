#pragma once

#include <array>
#include <cstdint>

namespace pbr {

/** A finger event as the crop page sees it (SDL_EVENT_FINGER_DOWN / MOTION / UP / CANCELED). */
enum class FingerEventKind { Down, Move, Up, Cancel };

/**
 * What one finger event did to the picture (PLAN-avatar-edit item 8). Same screen convention as AvatarCropFrame: dp,
 * x right, y DOWN, a positive angle is clockwise as the user sees it. Apply in this order:
 * `frame.Pan(pan_dx, pan_dy)`, then `frame.Pinch(focus, scale, rotation)` — the pan carries the picture point under
 * the old midpoint to the new midpoint (`focus`), the pinch then scales and turns about it.
 */
struct FingerGestureDelta {
  double pan_dx = 0.0;
  double pan_dy = 0.0;
  /** Ratio of the two fingers' distance after / before; 1 with one finger. */
  double scale = 1.0;
  /** Change of the angle of the line between the two fingers, radians in (-pi, pi]; 0 with one finger. */
  double rotation = 0.0;
  /** The two fingers' midpoint after the event (the one finger's position with one finger). */
  double focus_x = 0.0;
  double focus_y = 0.0;

  bool HasPinch() const { return scale != 1.0 || rotation != 0.0; }
};

/**
 * Tracks up to two fingers and turns each event into a delta (no GUI or SDL type; positions in any dp space the caller
 * keeps fixed, e.g. relative to the viewport centre).
 * - One finger: its movement is a pan.
 * - Two fingers: the midpoint's movement is a pan, the distance ratio a scale, the angle change a turn, about the
 *   midpoint. While the fingers are closer than kMinPinchDistanceDp (before or after the event) scale and turn hold
 *   (1 / 0) so a near-zero distance cannot blow the scale up; they resume once the fingers separate.
 * - A third and later finger does not move the picture, but its position is remembered (up to kMaxExtraFingers).
 *   When a tracked finger lifts, the oldest such finger still down joins the gesture from its last position, without a
 *   jump; one that lifts before that is forgotten.
 * - Lifting one of two fingers with no other down: the other carries on as a one-finger pan from where it is, without
 *   a jump.
 * - A Down for an id already tracked moves that finger without a delta; an Up / Move for an unknown id is ignored.
 * - Cancel (any id) or Reset(): every tracked finger is forgotten.
 */
class TwoFingerGesture {
public:
  static constexpr double kMinPinchDistanceDp = 8.0;
  static constexpr int kMaxExtraFingers = 3;

  FingerGestureDelta OnEvent(int64_t finger_id, double x_dp, double y_dp, FingerEventKind kind);
  void Reset();
  int TrackedCount() const;

private:
  struct Finger {
    bool active = false;
    int64_t id = 0;
    double x = 0.0;
    double y = 0.0;
  };
  Finger* Find(int64_t finger_id);
  Finger* FindExtra(int64_t finger_id);
  void DropExtra(const Finger* extra);

  std::array<Finger, 2> fingers_{};
  /** Fingers down beyond the first two, oldest first; the first `extra_count_` are in use. */
  std::array<Finger, kMaxExtraFingers> extras_{};
  int extra_count_ = 0;
};

} // namespace pbr
