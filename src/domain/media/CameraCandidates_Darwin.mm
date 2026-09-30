#include "domain/media/CameraCandidates.h"

#import <AVFoundation/AVFoundation.h>

namespace pbr {

// macOS: Continuity Camera devices (SDL names cameras by AVCaptureDevice.localizedName).
bool IsBorrowedCameraName(const char* name) {
  if (!name || !name[0]) {
    return false;
  }
  if (@available(macOS 14.0, *)) {
    @autoreleasepool {
      NSString* wanted = [NSString stringWithUTF8String:name];
      AVCaptureDeviceDiscoverySession* session = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeContinuityCamera, AVCaptureDeviceTypeExternal ]
                                mediaType:AVMediaTypeVideo
                                 position:AVCaptureDevicePositionUnspecified];
      for (AVCaptureDevice* device in session.devices) {
        if (device.isContinuityCamera && [device.localizedName isEqualToString:wanted]) {
          return true;
        }
      }
    }
  }
  return false;
}

} // namespace pbr
