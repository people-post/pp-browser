#include "gui/avatar/AvatarCropTouch.h"

namespace pbr {

namespace {
bool IsIndirectDevice(const SDL_TouchID touch) {
  if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
    return false; // no touch devices are known before the video system starts (a test, or teardown)
  }
  const SDL_TouchDeviceType type = SDL_GetTouchDeviceType(touch);
  return type == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE || type == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}
} // namespace

bool AvatarCropTouch::OnSdlEvent(const SDL_Event& event, const CropTouchSpace& space) {
  FingerEventKind kind;
  switch (event.type) {
    case SDL_EVENT_FINGER_DOWN:
      kind = FingerEventKind::Down;
      break;
    case SDL_EVENT_FINGER_MOTION:
      kind = FingerEventKind::Move;
      break;
    case SDL_EVENT_FINGER_UP:
      kind = FingerEventKind::Up;
      break;
    case SDL_EVENT_FINGER_CANCELED:
      kind = FingerEventKind::Cancel;
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      Clear();
      return false;
    default:
      return false;
  }
  const SDL_TouchFingerEvent& finger = event.tfinger;
  if (IsIndirectDevice(finger.touchID)) {
    return false;
  }
  const float ratio = space.px_per_dp > 0.0f ? space.px_per_dp : 1.0f;
  const double x_dp = static_cast<double>(finger.x) * space.window_w_px / ratio;
  const double y_dp = static_cast<double>(finger.y) * space.window_h_px / ratio;
  const bool inside = x_dp >= space.box_x && x_dp < space.box_x + space.box_w && y_dp >= space.box_y &&
                      y_dp < space.box_y + space.box_h;
  const auto id = static_cast<int64_t>(finger.fingerID);
  const CropFingerRoute route = ownership_.OnFinger(kind, id, inside);
  if (kind == FingerEventKind::Cancel) {
    // The ownership forgot every finger (owned or not): the gesture and the unapplied movement go with them.
    gesture_.Reset();
    accumulator_.Reset();
    return route == CropFingerRoute::Own;
  }
  if (route == CropFingerRoute::Pass) {
    return false;
  }
  // Relative to the circle's centre, the space AvatarCropFrame works in.
  const double rel_x = x_dp - (space.box_x + space.box_w * 0.5);
  const double rel_y = y_dp - (space.box_y + space.box_h * 0.5);
  const FingerGestureDelta delta = gesture_.OnEvent(id, rel_x, rel_y, kind);
  if (kind == FingerEventKind::Move) {
    accumulator_.Add(delta);
  }
  return true;
}

void AvatarCropTouch::Clear() {
  ownership_.Clear();
  gesture_.Reset();
  accumulator_.Reset();
}

void AvatarCropTouch::SetPaused(const bool paused) {
  ownership_.SetPaused(paused);
  if (paused) {
    gesture_.Reset();
    accumulator_.Reset();
  }
}

} // namespace pbr
