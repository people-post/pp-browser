#include "domain/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

namespace pbr {
namespace {

int InterfaceOrientationDegrees() {
  UIInterfaceOrientation io = UIInterfaceOrientationUnknown;
  if (@available(iOS 13.0, *)) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) {
        continue;
      }
      UIWindowScene* window_scene = (UIWindowScene*)scene;
      if (window_scene.activationState == UISceneActivationStateForegroundActive ||
          window_scene.activationState == UISceneActivationStateForegroundInactive) {
        io = window_scene.interfaceOrientation;
        break;
      }
    }
  }
  if (io == UIInterfaceOrientationUnknown) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    io = UIApplication.sharedApplication.statusBarOrientation;
#pragma clang diagnostic pop
  }

  switch (io) {
  case UIInterfaceOrientationLandscapeRight:
    return 90;
  case UIInterfaceOrientationPortraitUpsideDown:
    return 180;
  case UIInterfaceOrientationLandscapeLeft:
    return 270;
  case UIInterfaceOrientationPortrait:
  default:
    return 0;
  }
}

int Normalize90(int deg) {
  deg %= 360;
  if (deg < 0) {
    deg += 360;
  }
  const int snapped = ((deg + 45) / 90) * 90;
  return snapped % 360;
}

} // namespace

CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id) {
  CameraCaptureTransform t;
  t.encode_width = 360;
  t.encode_height = 640;

  // AVFoundation does not expose Android-style SENSOR_ORIENTATION. Built-in iPhone
  // cameras use the same conventional angles; SDL CoreMedia leaves connection
  // videoOrientation unset, so buffers need the same compensation as Android.
  const SDL_CameraPosition pos = SDL_GetCameraPosition(camera_id);
  const bool front = (pos != SDL_CAMERA_POSITION_BACK_FACING);
  const int sensor_deg = front ? 270 : 90;
  const int display_deg = Normalize90(InterfaceOrientationDegrees());

  int rotate_cw = 0;
  if (front) {
    rotate_cw = (sensor_deg + display_deg) % 360;
  } else {
    rotate_cw = (sensor_deg - display_deg + 360) % 360;
  }
  t.rotate_cw = Normalize90(rotate_cw);

  if (t.rotate_cw == 0 || t.rotate_cw == 180) {
    t.encode_width = 640;
    t.encode_height = 360;
  }
  return t;
}

} // namespace pbr

#endif // TARGET_OS_IPHONE
