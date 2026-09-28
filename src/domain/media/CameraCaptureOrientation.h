#pragma once

#include <SDL3/SDL.h>

namespace pbr {

/** How to turn an SDL camera buffer into upright encode/preview frames. */
struct CameraCaptureTransform {
  /** Clockwise degrees (0/90/180/270) to rotate sensor buffers to upright. */
  int rotate_cw = 0;
  /** Even encode size after rotation (cover-crop target). */
  int encode_width = 640;
  int encode_height = 360;
  /** Opened camera faces the user (iOS: rotation follows the display the other way than the back camera). */
  bool front_facing = false;
};

/**
 * Current display rotation, clockwise degrees (0/90/180/270). Call on the UI thread (iOS reads
 * UIKit: the phone's physical orientation, so a portrait-locked UI still sends upright video); pass
 * the result to ResolveCameraCaptureTransform / CameraFrameRotateCw on any thread.
 */
int CameraDisplayRotationDegrees();

/**
 * Resolve capture transform for an SDL camera id. Any thread (the media device thread opens cameras).
 * Android: ACAMERA_SENSOR_ORIENTATION + display rotation (CameraX compensation).
 * iOS: conventional sensor angles + interface/display orientation.
 * Desktop: identity + landscape encode.
 */
CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id, int display_rotation_deg);

/**
 * Per-frame clockwise rotation for a camera opened with `opened`, given the current display
 * rotation (may differ from the one at open). iOS: follows the phone's orientation live; Android /
 * desktop: `opened.rotate_cw` (fixed at open). Any thread.
 */
int CameraFrameRotateCw(const CameraCaptureTransform& opened, int current_display_rotation_deg);

/**
 * Clockwise rotation for the LOCAL preview, which is drawn on the device's own screen: upright
 * relative to the screen, not the world. iOS (portrait-locked UI): the portrait rotation, so the
 * preview stays head-up on the screen while the sent frame follows the phone's physical
 * orientation. Android / desktop: same as the sent frame (`opened.rotate_cw`). Any thread.
 */
int CameraPreviewRotateCw(const CameraCaptureTransform& opened);

/**
 * iOS: clockwise degrees to make a CoreMedia camera buffer upright, from camera facing and the
 * interface rotation (CW degrees). Both built-in iPhone cameras need 90° CW in portrait (matches
 * SDL CoreMedia's SDL_PROP_SURFACE_ROTATION_FLOAT and WebRTC); the front camera turns with the
 * display, the back camera against it. Pure — unit-tested on every platform.
 */
inline int IosCameraRotateCw(bool front_facing, int display_rotation_deg) {
  int deg = display_rotation_deg % 360;
  if (deg < 0) {
    deg += 360;
  }
  const int display = (((deg + 45) / 90) * 90) % 360;
  constexpr int kPortraitSensorDeg = 90;
  return front_facing ? (kPortraitSensorDeg + display) % 360 : (kPortraitSensorDeg - display + 360) % 360;
}

} // namespace pbr
