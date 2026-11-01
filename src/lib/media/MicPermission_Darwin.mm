#include "lib/media/MicPermission.h"

#import <AVFoundation/AVFoundation.h>

namespace pbr {

MicPermission MicPermissionState() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
  case AVAuthorizationStatusAuthorized:
    return MicPermission::Granted;
  case AVAuthorizationStatusNotDetermined:
    return MicPermission::NotDetermined;
  case AVAuthorizationStatusDenied:
  case AVAuthorizationStatusRestricted:
    return MicPermission::Denied;
  }
  return MicPermission::Denied;
}

void RequestMicPermission(std::function<void(bool granted)> done) {
  if (MicPermissionState() != MicPermission::NotDetermined) {
    if (done) {
      done(MicPermissionState() == MicPermission::Granted);
    }
    return;
  }
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                           completionHandler:^(BOOL granted) {
                             if (done) {
                               done(granted == YES);
                             }
                           }];
}

} // namespace pbr
