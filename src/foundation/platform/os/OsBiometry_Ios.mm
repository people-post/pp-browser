#include "foundation/platform/os/OsBiometry.h"

#import <LocalAuthentication/LocalAuthentication.h>

namespace pbr::os {

// The platform target compiles Objective-C++ without ARC: the context is released by hand.
Biometry BiometryKind() {
  @autoreleasepool {
    LAContext* context = [[LAContext alloc] init];
    NSError* error = nil;
    Biometry kind = Biometry::None;
    if ([context canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&error]) {
      switch (context.biometryType) {
      case LABiometryTypeFaceID:
        kind = Biometry::Face;
        break;
      case LABiometryTypeTouchID:
        kind = Biometry::Touch;
        break;
      default:
        break; // LABiometryTypeNone, and kinds this build does not name (Optic ID): no switch
      }
    }
    [context release];
    return kind;
  }
}

} // namespace pbr::os
