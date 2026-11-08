// APNs registration, the push callbacks and the notification-center delegate (the relay's
// call alert (P012) and the app's own banners (IosLocalNotifier)). SDL3 on iOS 13+ starts
// UIApplicationMain with SDLUIKitSceneDelegate, so a SDLUIKitDelegate subclass is never
// instantiated; instead hook in from +load and add the callbacks to SDL's delegate classes.

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>
#import <objc/runtime.h>

#include "foundation/platform/IosLocalNotifier.h"
#include "foundation/platform/IosPushDeviceRegistrar.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"

#include <string>

@interface PpNotificationCenterDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@interface PpApnsBootstrap : NSObject
@end

namespace {

/** The relay's call alert (P012) carries `type: call_wake`. */
bool IsCallWake(NSDictionary* userInfo) {
  id raw = userInfo[@"type"];
  return [raw isKindOfClass:[NSString class]] && [(NSString*)raw isEqualToString:@"call_wake"];
}

} // namespace

@implementation PpNotificationCenterDelegate

// In the foreground: the app rings and shows messages itself, so the alert is not shown; a call
// alert still makes it look for the ring now.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
  (void)center;
  if (IsCallWake(notification.request.content.userInfo)) {
    pbr::BackgroundSyncScheduler::Instance().RequestUrgentWakeSync();
  }
  completionHandler(UNNotificationPresentationOptionNone);
}

// A tap: a call alert brings the app up to ring; a message banner opens its chat.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  NSDictionary* userInfo = response.notification.request.content.userInfo;
  id hint_raw = userInfo[[NSString stringWithUTF8String:pbr::kIosNotificationWakeHintKey]];
  const std::string wake_hint = ([hint_raw isKindOfClass:[NSString class]] && [(NSString*)hint_raw length] > 0)
                                    ? std::string([(NSString*)hint_raw UTF8String])
                                    : std::string();
  if (IsCallWake(userInfo)) {
    pbr::DispatchIosNotificationActivation({}, /*call_wake=*/true, wake_hint);
    pbr::BackgroundSyncScheduler::Instance().RequestUrgentWakeSync();
  } else {
    id thread = userInfo[[NSString stringWithUTF8String:pbr::kIosNotificationThreadKey]];
    if ([thread isKindOfClass:[NSString class]] && [(NSString*)thread length] > 0) {
      pbr::DispatchIosNotificationActivation(std::string([(NSString*)thread UTF8String]));
    } else if (!wake_hint.empty()) {
      pbr::DispatchIosNotificationActivation({}, /*call_wake=*/false, wake_hint);
      pbr::BackgroundSyncScheduler::Instance().RequestWakeSync();
    }
  }
  completionHandler();
}

@end

namespace {

void AddAppDelegateMethods(Class cls) {
  if (cls == Nil) {
    return;
  }
  // class_addMethod leaves any method the class already implements untouched.
  class_addMethod(cls, @selector(application:didRegisterForRemoteNotificationsWithDeviceToken:),
                  imp_implementationWithBlock(^(id /*self*/, UIApplication* /*app*/, NSData* token) {
                    if (token == nil || token.length == 0) {
                      return;
                    }
                    const unsigned char* bytes = static_cast<const unsigned char*>(token.bytes);
                    std::string hex;
                    hex.reserve(token.length * 2);
                    static const char* kHex = "0123456789abcdef";
                    for (NSUInteger i = 0; i < token.length; ++i) {
                      hex.push_back(kHex[(bytes[i] >> 4) & 0xF]);
                      hex.push_back(kHex[bytes[i] & 0xF]);
                    }
                    pbr::SetIosCachedApnsToken(std::move(hex));
                  }),
                  "v@:@@");
  class_addMethod(cls, @selector(application:didFailToRegisterForRemoteNotificationsWithError:),
                  imp_implementationWithBlock(^(id /*self*/, UIApplication* /*app*/, NSError* error) {
                    NSLog(@"[apns] register failed: %@", error.localizedDescription);
                  }),
                  "v@:@@");
  class_addMethod(
      cls, @selector(application:didReceiveRemoteNotification:fetchCompletionHandler:),
      imp_implementationWithBlock(^(id /*self*/, UIApplication* /*app*/, NSDictionary* userInfo,
                                    void (^completionHandler)(UIBackgroundFetchResult)) {
        if (IsCallWake(userInfo)) {
          pbr::BackgroundSyncScheduler::Instance().RequestUrgentWakeSync();
        } else {
          // inbox_wake or unknown opaque wake: force inbox sync.
          pbr::BackgroundSyncScheduler::Instance().RequestWakeSync();
        }
        if (completionHandler) {
          completionHandler(UIBackgroundFetchResultNewData);
        }
      }),
      "v@:@@@?");
}

PpNotificationCenterDelegate* g_center_delegate = nil;

} // namespace

@implementation PpApnsBootstrap

+ (void)load {
  // Before UIApplicationMain: the delegate classes must have the methods when it is set.
  AddAppDelegateMethods(NSClassFromString(@"SDLUIKitSceneDelegate"));
  AddAppDelegateMethods(NSClassFromString(@"SDLUIKitDelegate"));
  g_center_delegate = [[PpNotificationCenterDelegate alloc] init];
  // Set before launch finishes so a tap that launched the app is delivered.
  [UNUserNotificationCenter currentNotificationCenter].delegate = g_center_delegate;
  // UIKit has no will-finish notification; this one is posted as SDL's
  // application:didFinishLaunchingWithOptions: returns, where the work used to run.
  [[NSNotificationCenter defaultCenter]
      addObserverForName:UIApplicationDidFinishLaunchingNotification
                  object:nil
                   queue:nil
              usingBlock:^(NSNotification* /*note*/) {
                UIApplication* application = [UIApplication sharedApplication];
                [[UNUserNotificationCenter currentNotificationCenter]
                    requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                                     UNAuthorizationOptionSound |
                                                     UNAuthorizationOptionBadge)
                                  completionHandler:^(BOOL /*granted*/, NSError* /*error*/) {
                                    dispatch_async(dispatch_get_main_queue(), ^{
                                      [[UIApplication sharedApplication]
                                          registerForRemoteNotifications];
                                    });
                                  }];
                // Silent wakes do not require the alert grant.
                [application registerForRemoteNotifications];
              }];
}

@end

namespace pbr {
void LinkIosApnsAppDelegate() {}
} // namespace pbr

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
