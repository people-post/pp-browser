#include "lib/media/CameraCaptureOrientation.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(__ANDROID__) && !(defined(__APPLE__) && TARGET_OS_IPHONE)

namespace pbr {

bool CameraRotationFollowsDevice() {
  return false;
}

void StopCameraOrientationTracking() {}

int CameraDisplayRotationDegrees() {
  return 0;
}

CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID /*camera_id*/, int /*display_rotation_deg*/) {
  // Desktop only (the phones have their own backends): the camera's own frame size.
  return CameraCaptureTransform{};
}

int CameraFrameRotateCw(const CameraCaptureTransform& opened, int /*current_display_rotation_deg*/) {
  return opened.rotate_cw;
}

int CameraPreviewRotateCw(const CameraCaptureTransform& opened) {
  return opened.rotate_cw;
}

} // namespace pbr

#endif
