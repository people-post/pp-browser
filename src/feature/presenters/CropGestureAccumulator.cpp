#include "feature/presenters/CropGestureAccumulator.h"

namespace pbr {

void CropGestureAccumulator::Add(const FingerGestureDelta& delta) {
  pending_ = true;
  pan_dx_ += delta.pan_dx;
  pan_dy_ += delta.pan_dy;
  scale_ *= delta.scale;
  rotation_ += delta.rotation;
  focus_x_ = delta.focus_x;
  focus_y_ = delta.focus_y;
}

void CropGestureAccumulator::Reset() {
  *this = CropGestureAccumulator();
}

bool CropGestureAccumulator::ApplyTo(AvatarCropFrame& frame) {
  if (!pending_) {
    return false;
  }
  const double pan_dx = pan_dx_;
  const double pan_dy = pan_dy_;
  const double scale = scale_;
  const double rotation = rotation_;
  const AvatarCropFrame::Point focus{focus_x_, focus_y_};
  Reset();
  frame.Pan(pan_dx, pan_dy);
  if (scale != 1.0 || rotation != 0.0) {
    frame.Pinch(focus, scale, rotation);
  }
  return true;
}

} // namespace pbr
