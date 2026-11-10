#include "feature/presenters/TwoFingerGesture.h"

#include <cmath>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;

/** (-pi, pi]. */
double WrapAngle(double a) {
  a = std::remainder(a, 2.0 * kPi);
  return a <= -kPi ? a + 2.0 * kPi : a;
}

} // namespace

TwoFingerGesture::Finger* TwoFingerGesture::Find(const int64_t finger_id) {
  for (Finger& f : fingers_) {
    if (f.active && f.id == finger_id) {
      return &f;
    }
  }
  return nullptr;
}

int TwoFingerGesture::TrackedCount() const {
  int n = 0;
  for (const Finger& f : fingers_) {
    n += f.active ? 1 : 0;
  }
  return n;
}

void TwoFingerGesture::Reset() {
  fingers_ = {};
}

FingerGestureDelta TwoFingerGesture::OnEvent(const int64_t finger_id, const double x_dp, const double y_dp,
                                             const FingerEventKind kind) {
  FingerGestureDelta delta;
  delta.focus_x = x_dp;
  delta.focus_y = y_dp;
  if (kind == FingerEventKind::Cancel) {
    Reset();
    return delta;
  }
  if (!std::isfinite(x_dp) || !std::isfinite(y_dp)) {
    return delta;
  }
  Finger* const finger = Find(finger_id);
  switch (kind) {
    case FingerEventKind::Down:
      if (finger) {
        finger->x = x_dp;
        finger->y = y_dp;
        return delta;
      }
      for (Finger& f : fingers_) {
        if (!f.active) {
          f = {true, finger_id, x_dp, y_dp};
          return delta;
        }
      }
      return delta; // a third finger
    case FingerEventKind::Up:
      if (finger) {
        finger->active = false;
      }
      return delta;
    case FingerEventKind::Move:
      break;
    case FingerEventKind::Cancel:
      return delta;
  }
  if (!finger) {
    return delta;
  }
  Finger* other = nullptr;
  for (Finger& f : fingers_) {
    if (f.active && &f != finger) {
      other = &f;
    }
  }
  if (!other) {
    delta.pan_dx = x_dp - finger->x;
    delta.pan_dy = y_dp - finger->y;
    finger->x = x_dp;
    finger->y = y_dp;
    return delta;
  }
  const double old_mx = (finger->x + other->x) / 2.0;
  const double old_my = (finger->y + other->y) / 2.0;
  const double old_dx = finger->x - other->x;
  const double old_dy = finger->y - other->y;
  finger->x = x_dp;
  finger->y = y_dp;
  const double new_mx = (finger->x + other->x) / 2.0;
  const double new_my = (finger->y + other->y) / 2.0;
  const double new_dx = finger->x - other->x;
  const double new_dy = finger->y - other->y;
  delta.pan_dx = new_mx - old_mx;
  delta.pan_dy = new_my - old_my;
  delta.focus_x = new_mx;
  delta.focus_y = new_my;
  const double old_len = std::hypot(old_dx, old_dy);
  const double new_len = std::hypot(new_dx, new_dy);
  if (old_len >= kMinPinchDistanceDp && new_len >= kMinPinchDistanceDp) {
    delta.scale = new_len / old_len;
    // atan2 on a y-down screen: a growing angle is a clockwise turn, the same sign as AvatarCropFrame's theta.
    delta.rotation = WrapAngle(std::atan2(new_dy, new_dx) - std::atan2(old_dy, old_dx));
  }
  return delta;
}

} // namespace pbr
