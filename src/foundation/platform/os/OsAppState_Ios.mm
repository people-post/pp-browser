#include "foundation/platform/os/OsAppState.h"

#import <UIKit/UIKit.h>

namespace pbr::os {

bool AppShownNow() {
  return UIApplication.sharedApplication.applicationState != UIApplicationStateBackground;
}

const char* AppStateName() {
  switch (UIApplication.sharedApplication.applicationState) {
    case UIApplicationStateActive:
      return "active";
    case UIApplicationStateInactive:
      return "inactive";
    case UIApplicationStateBackground:
      return "background";
  }
  return "unknown";
}

} // namespace pbr::os
