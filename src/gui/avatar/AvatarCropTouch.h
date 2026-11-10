#pragma once

#include "feature/presenters/AvatarCropFrame.h"
#include "feature/presenters/CropFingerOwnership.h"
#include "feature/presenters/CropGestureAccumulator.h"
#include "feature/presenters/TwoFingerGesture.h"

#include <SDL3/SDL.h>

namespace pbr {

/** Where the crop element is and how the window maps to dp, as the touch routing needs it (all absolute, in dp). */
struct CropTouchSpace {
  /** The window's size in the UI context's pixels (`tfinger.x / y` are 0..1 of it). */
  float window_w_px = 0.0f;
  float window_h_px = 0.0f;
  /** Context pixels per dp. */
  float px_per_dp = 1.0f;
  /** The crop element's box. Its centre is the viewport circle's centre. */
  float box_x = 0.0f;
  float box_y = 0.0f;
  float box_w = 0.0f;
  float box_h = 0.0f;
};

/**
 * The avatar crop page's touch side, driven by raw SDL events from SdlAppEvents::PreProcess (before the UI library,
 * which would turn every finger into a mouse press). The rule (see CropFingerOwnership for the full text):
 * - a finger-down inside the element's box is owned while nothing is tracked; outside it passes (the buttons work);
 * - while any finger is tracked every new finger-down is owned wherever it lands; the first two feed the gesture;
 * - an owned finger's motion / up / cancel are swallowed (returns true), nothing else is;
 * - focus lost, minimised, going to the background clear everything; paused (a PIN pad is over the page) passes all.
 * Fingers from an indirect device (a Mac trackpad) are not touches on the picture and pass.
 * Movement is not applied here: `Apply` sums what arrived since the last call and applies it once (the page ticks it
 * once per frame), so a two-finger twist cannot drift the scale.
 * UI thread only.
 */
class AvatarCropTouch {
public:
  /** True when the event was taken and the UI library must not see it. */
  bool OnSdlEvent(const SDL_Event& event, const CropTouchSpace& space);
  /** Forget every tracked finger and anything not applied yet. */
  void Clear();
  void SetPaused(bool paused);
  bool paused() const { return ownership_.paused(); }
  bool Tracking() const { return ownership_.Tracking(); }
  /** Applies the movement summed since the last call to `frame`; true when the frame was touched. */
  bool Apply(AvatarCropFrame& frame) { return accumulator_.ApplyTo(frame); }

private:
  CropFingerOwnership ownership_;
  TwoFingerGesture gesture_;
  CropGestureAccumulator accumulator_;
};

} // namespace pbr
