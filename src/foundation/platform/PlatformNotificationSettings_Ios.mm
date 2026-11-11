// iOS: reads PP's notification banner style and opens its page in the system Settings. A ringtone that plays with a
// call notification stops when a "Temporary" banner goes away; "Persistent" keeps it ringing (DEV/PLAN-call-wake-placeholder.md).
#include "foundation/platform/PlatformNotificationSettings.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "common/runtime/AppRuntime.h"

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

#include <utility>

namespace pbr {

void QueryBannerStyle(std::function<void(BannerStyle)> done) {
  if (!done) {
    return;
  }
  [[UNUserNotificationCenter currentNotificationCenter]
      getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
        BannerStyle style = BannerStyle::Unknown;
        const bool granted = settings.authorizationStatus == UNAuthorizationStatusAuthorized ||
                             settings.authorizationStatus == UNAuthorizationStatusProvisional ||
                             settings.authorizationStatus == UNAuthorizationStatusEphemeral;
        if (!granted) {
          style = BannerStyle::None;
        } else {
          switch (settings.alertStyle) {
            case UNAlertStyleBanner:
              style = BannerStyle::Temporary;
              break;
            case UNAlertStyleAlert:
              style = BannerStyle::Persistent;
              break;
            case UNAlertStyleNone:
              style = BannerStyle::None;
              break;
          }
        }
        pbr::logging::getLogger("LocalNotifier").info
            << "banner style: alertStyle=" << static_cast<long>(settings.alertStyle)
            << " authorization=" << static_cast<long>(settings.authorizationStatus)
            << " mapped=" << static_cast<int>(style);
        AppRuntime::PostUI([done, style]() { done(style); });
      }];
}

bool OpenNotificationSettings() {
  __block bool opened = false;
  void (^open)(void) = ^{
    NSString* address = UIApplicationOpenSettingsURLString;
    if (@available(iOS 16.0, *)) {
      address = UIApplicationOpenNotificationSettingsURLString;
    }
    NSURL* url = [NSURL URLWithString:address];
    if (url == nil || ![[UIApplication sharedApplication] canOpenURL:url]) {
      url = [NSURL URLWithString:UIApplicationOpenSettingsURLString];
    }
    if (url == nil) {
      return;
    }
    opened = true;
    [[UIApplication sharedApplication] openURL:url options:@{} completionHandler:nil];
  };
  if ([NSThread isMainThread]) {
    open();
  } else {
    dispatch_sync(dispatch_get_main_queue(), open);
  }
  return opened;
}

// The Android notification tips: nothing to say on iOS (its one tip is the banner style above).
void QueryNotificationHealth(std::function<void(NotificationHealth)> done) {
  if (!done) {
    return;
  }
  AppRuntime::PostUI([done = std::move(done)]() { done(NotificationHealth{}); });
}

bool OpenNotificationFix(NotificationFix) {
  return false;
}

} // namespace pbr
