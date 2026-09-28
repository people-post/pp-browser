#include "domain/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>

#include <cmath>

#include <SDL3/SDL.h>

namespace pbr {

int CameraDisplayRotationDegrees() {
  // The app is portrait-locked and the sensor turns with the phone, so we need the phone's
  // PHYSICAL orientation. UIDevice.orientation stays Portrait while the user's Control Center
  // "Portrait Orientation Lock" is on (device test 2026-09-28: the peer always saw the picture
  // turned with the phone), so read gravity from CoreMotion instead — like FaceTime.
  // Degrees are CW display rotation, matching IosCameraRotateCw's table: portrait 0, home side
  // right (UIDeviceOrientationLandscapeLeft) 90, upside down 180, home side left 270.
  // UI thread only (called from the call UI tick while the camera is on).
  static CMMotionManager* motion = nil;
  static int last_valid_deg = 0;
  if (motion == nil) {
    motion = [[CMMotionManager alloc] init];
    motion.deviceMotionUpdateInterval = 0.2;
    if (motion.deviceMotionAvailable) {
      [motion startDeviceMotionUpdates];
    }
  }
  CMDeviceMotion* dm = motion.deviceMotion;
  if (dm == nil) {
    return last_valid_deg;  // not started yet / unavailable (simulator)
  }
  const double gx = dm.gravity.x;
  const double gy = dm.gravity.y;
  const double gz = dm.gravity.z;
  // Flat on a table (gravity mostly through the screen): keep the last upright orientation.
  // Hysteresis: the new axis must clearly dominate so ~45° doesn't flicker.
  constexpr double kFlat = 0.8;
  constexpr double kMargin = 0.2;
  int deg = last_valid_deg;
  if (std::fabs(gz) < kFlat) {
    if (std::fabs(gy) > std::fabs(gx) + kMargin) {
      deg = gy < 0 ? 0 : 180;
    } else if (std::fabs(gx) > std::fabs(gy) + kMargin) {
      deg = gx < 0 ? 90 : 270;
    }
  }
  if (deg != last_valid_deg) {
    NSLog(@"CameraOrientation: physical rotation %d -> %d (gravity x=%.2f y=%.2f z=%.2f)", last_valid_deg, deg,
          gx, gy, gz);
    last_valid_deg = deg;
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
