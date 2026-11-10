#pragma once

#include "feature/presenters/AvatarCropFrame.h"
#include "feature/presenters/TwoFingerGesture.h"

namespace pbr {

/**
 * Sums the finger deltas that arrive between two frames and applies them to the frame once (PLAN-avatar-edit item 9).
 * SDL reports one finger per event, so a pure two-finger twist reaches TwoFingerGesture as "finger A moved, then finger
 * B moved": each step alone changes the fingers' distance, and applying the steps one by one lets the clamp at scale 1
 * (or 5) swallow the first half and keep the second, so the picture drifts in size. The per-step ratios multiply to
 * the true ratio (d_end / d_start, 1 for a twist), so the sum is exact: pans add up, scales multiply, turns add up,
 * the focus is the last one. `ApplyTo` then does what the gesture header says: `Pan`, then `Pinch`.
 */
class CropGestureAccumulator {
public:
  void Add(const FingerGestureDelta& delta);
  bool Pending() const { return pending_; }
  void Reset();
  /** Applies what was added (nothing when nothing is pending) and clears it; true when the frame was touched. */
  bool ApplyTo(AvatarCropFrame& frame);

private:
  bool pending_ = false;
  double pan_dx_ = 0.0;
  double pan_dy_ = 0.0;
  double scale_ = 1.0;
  double rotation_ = 0.0;
  double focus_x_ = 0.0;
  double focus_y_ = 0.0;
};

} // namespace pbr
