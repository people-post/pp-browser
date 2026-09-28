#include "domain/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <SDL3/SDL.h>

namespace pbr {

int CameraDisplayRotationDegrees() {
  // The app is portrait-locked, so the interface orientation never changes; the sensor turns with
  // the phone, so use its PHYSICAL orientation (UI thread: UIDevice is main-thread API).
  // UIDeviceOrientationLandscapeLeft (home side right) == UIInterfaceOrientationLandscapeRight →
  // 90° CW, the same numbers as IosCameraRotateCw's table.
  static bool generating = false;
  static int last_valid_deg = 0;
  UIDevice* device = UIDevice.currentDevice;
  if (!generating) {
    [device beginGeneratingDeviceOrientationNotifications];
    generating = true;
  }
  switch (device.orientation) {
  case UIDeviceOrientationPortrait:
    last_valid_deg = 0;
    break;
  case UIDeviceOrientationLandscapeLeft:
    last_valid_deg = 90;
    break;
  case UIDeviceOrientationPortraitUpsideDown:
    last_valid_deg = 180;
    break;
  case UIDeviceOrientationLandscapeRight:
    last_valid_deg = 270;
    break;
  default: // FaceUp / FaceDown / Unknown: keep the last upright orientation
    break;
  }
  return last_valid_deg;
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
  t.front_facing = front;
  t.rotate_cw = IosCameraRotateCw(front, display_rotation_deg);

  if (t.rotate_cw == 0 || t.rotate_cw == 180) {
    t.encode_width = 640;
    t.encode_height = 360;
  }
  return t;
}

int CameraFrameRotateCw(const CameraCaptureTransform& opened, int current_display_rotation_deg) {
  return IosCameraRotateCw(opened.front_facing, current_display_rotation_deg);
}

} // namespace pbr

#endif // TARGET_OS_IPHONE
