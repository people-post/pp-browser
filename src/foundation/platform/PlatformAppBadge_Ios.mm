// iOS: the red number on PP's home screen icon. It needs the badge permission (asked at launch, granted
// asynchronously): a call made before the answer is ignored by the system, so the caller sends the number again later.
#include "foundation/platform/PlatformAppBadge.h"

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

namespace pbr {

void SetAppBadgeCount(int count) {
  const NSInteger shown = count > 0 ? count : 0;
  if (@available(iOS 16.0, *)) {
    [[UNUserNotificationCenter currentNotificationCenter] setBadgeCount:shown withCompletionHandler:nil];
    return;
  }
  dispatch_async(dispatch_get_main_queue(), ^{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [UIApplication sharedApplication].applicationIconBadgeNumber = shown;
#pragma clang diagnostic pop
  });
}

} // namespace pbr
