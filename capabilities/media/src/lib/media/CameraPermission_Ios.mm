#include "lib/media/CameraPermission.h"

#import <AVFoundation/AVFoundation.h>

namespace pbr {

CameraPermission CameraPermissionState() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized:
      return CameraPermission::Granted;
    case AVAuthorizationStatusNotDetermined:
      return CameraPermission::NotDetermined;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
      return CameraPermission::Denied;
  }
  return CameraPermission::Denied;
}

void RequestCameraPermission(std::function<void(bool granted)> done) {
  if (CameraPermissionState() != CameraPermission::NotDetermined) {
    if (done) {
      done(CameraPermissionState() == CameraPermission::Granted);
    }
    return;
  }
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
                             if (done) {
                               done(granted == YES);
                             }
                           }];
}

} // namespace pbr
