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
 * Resolve capture transform for an SDL camera id.
 * Android: ACAMERA_SENSOR_ORIENTATION + display rotation (CameraX compensation).
 * iOS: conventional sensor angles + interface/display orientation.
 * Desktop: identity + landscape encode.
 */
CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id);

} // namespace pbr
