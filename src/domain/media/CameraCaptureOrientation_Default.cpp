#include "domain/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)

#include "foundation/platform/Platform.h"

namespace pbr {

int CameraDisplayRotationDegrees() {
  return 0;
}

CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID /*camera_id*/, int /*display_rotation_deg*/) {
  CameraCaptureTransform t;
  if (Platform::IsMobile()) {
    t.encode_width = 360;
    t.encode_height = 640;
  }
  return t;
}

} // namespace pbr

#endif
