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
};

/**
 * Current display / interface rotation, clockwise degrees (0/90/180/270). Call on the UI thread
 * (iOS reads UIKit); pass the result to ResolveCameraCaptureTransform on any thread.
 */
int CameraDisplayRotationDegrees();

/**
 * Resolve capture transform for an SDL camera id. Any thread (the media device thread opens cameras).
 * Android: ACAMERA_SENSOR_ORIENTATION + display rotation (CameraX compensation).
 * iOS: conventional sensor angles + interface/display orientation.
 * Desktop: identity + landscape encode.
 */
CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id, int display_rotation_deg);

} // namespace pbr
