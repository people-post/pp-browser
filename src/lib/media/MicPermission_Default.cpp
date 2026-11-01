#include "lib/media/MicPermission.h"

namespace pbr {

MicPermission MicPermissionState() {
  return MicPermission::Granted;
}

void RequestMicPermission(std::function<void(bool granted)> done) {
  if (done) {
    done(true);
  }
}

} // namespace pbr
