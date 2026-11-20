#include "lib/media/CameraPermission.h"

namespace pbr {

CameraPermission CameraPermissionState() {
  return CameraPermission::Granted;
}

void RequestCameraPermission(std::function<void(bool granted)> done) {
  if (done) {
    done(true);
  }
}

} // namespace pbr
