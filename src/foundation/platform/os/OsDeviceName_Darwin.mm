#include "foundation/platform/os/OsDeviceName.h"

#include "common/DeviceName.h"

#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <SystemConfiguration/SystemConfiguration.h>
#endif

namespace pbr::os {

std::string SystemDeviceName() {
  std::string raw;
  @autoreleasepool {
#if TARGET_OS_IPHONE
    // iOS 16+ without the special entitlement gives only the generic name ("iPhone" / "iPad"): used as it is.
    if (const char* utf8 = [[[UIDevice currentDevice] name] UTF8String]) {
      raw = utf8;
    }
#else
    // The computer name of System Settings → General → Sharing. Not NSHost: its host-name lookup can block.
    if (CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr)) {
      if (const char* utf8 = [(__bridge NSString*)name UTF8String]) {
        raw = utf8;
      }
      CFRelease(name);
    }
#endif
  }
  return NormalizeDeviceName(raw);
}

} // namespace pbr::os
