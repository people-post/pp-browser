#include "domain/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

namespace pbr {

int CameraDisplayRotationDegrees() {
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

CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id, int display_rotation_deg) {
  CameraCaptureTransform t;
  t.encode_width = 360;
  t.encode_height = 640;

  // AVFoundation does not expose Android-style SENSOR_ORIENTATION and SDL CoreMedia leaves the
  // connection's videoOrientation unset, so we rotate buffers ourselves. Unlike Android (front
  // sensor 270°), both iPhone cameras need 90° CW in portrait — using 270° for the front camera
  // turned every iPhone selfie stream upside down (B51).
  const SDL_CameraPosition pos = SDL_GetCameraPosition(camera_id);
  const bool front = (pos != SDL_CAMERA_POSITION_BACK_FACING);
  t.rotate_cw = IosCameraRotateCw(front, display_rotation_deg);

  if (t.rotate_cw == 0 || t.rotate_cw == 180) {
    t.encode_width = 640;
    t.encode_height = 360;
  }
  return t;
}

} // namespace pbr

#endif // TARGET_OS_IPHONE
