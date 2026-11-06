#include "foundation/platform/os/OsAppState.h"

#import <UIKit/UIKit.h>

namespace pbr::os {

bool AppShownNow() {
  return UIApplication.sharedApplication.applicationState != UIApplicationStateBackground;
}

} // namespace pbr::os
