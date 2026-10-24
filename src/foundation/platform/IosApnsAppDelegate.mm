// Subclass SDL's UIApplicationDelegate so we can register for APNs and handle
// silent wakes without replacing SDL's launch / scene lifecycle. It is also the
// notification-center delegate: the relay's call alert (P012) and the app's own
// banners (IosLocalNotifier).
// Pattern: category overrides +getAppDelegateClassName (documented by SDL).

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

#include "foundation/platform/IosLocalNotifier.h"
#if defined(PP_IOS_CALLKIT)
#include "foundation/platform/IosCallSystemUi.h"
#endif
#include "foundation/platform/IosPushDeviceRegistrar.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"

#include <string>

// Redeclare SDL's delegate (private header not on the public include path).
@interface SDLUIKitDelegate : NSObject <UIApplicationDelegate>
+ (NSString*)getAppDelegateClassName;
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions;
@end

@interface PpSdlUIKitDelegate : SDLUIKitDelegate <UNUserNotificationCenterDelegate>
@end

namespace {

/** The relay's call alert (P012) carries `type: call_wake`. */
bool IsCallWake(NSDictionary* userInfo) {
  id raw = userInfo[@"type"];
  return [raw isKindOfClass:[NSString class]] && [(NSString*)raw isEqualToString:@"call_wake"];
}

} // namespace

@implementation PpSdlUIKitDelegate

- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)launchOptions {
  // Request alert permission for local banners; remote token registration is separate.
  UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
  // Before launch returns, so a tap that launched the app is delivered.
  center.delegate = self;
#if defined(PP_IOS_CALLKIT)
  // Likewise a VoIP push that launched it (P013).
  pbr::InstallIosCallSystemUi();
#endif
  [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound |
                                           UNAuthorizationOptionBadge)
                        completionHandler:^(BOOL /*granted*/, NSError* /*error*/) {
                          dispatch_async(dispatch_get_main_queue(), ^{
                            [[UIApplication sharedApplication] registerForRemoteNotifications];
                          });
                        }];
  // Also kick registration immediately (silent wakes do not require alert grant).
  [application registerForRemoteNotifications];
  return [super application:application didFinishLaunchingWithOptions:launchOptions];
}

- (void)application:(UIApplication*)application
    didRegisterForRemoteNotificationsWithDeviceToken:(NSData*)deviceToken {
  if (deviceToken == nil || deviceToken.length == 0) {
    return;
  }
  const unsigned char* bytes = static_cast<const unsigned char*>(deviceToken.bytes);
  std::string hex;
  hex.reserve(deviceToken.length * 2);
  static const char* kHex = "0123456789abcdef";
  for (NSUInteger i = 0; i < deviceToken.length; ++i) {
    hex.push_back(kHex[(bytes[i] >> 4) & 0xF]);
    hex.push_back(kHex[bytes[i] & 0xF]);
  }
  pbr::SetIosCachedApnsToken(std::move(hex));
}

- (void)application:(UIApplication*)application
    didFailToRegisterForRemoteNotificationsWithError:(NSError*)error {
  (void)application;
  NSLog(@"[apns] register failed: %@", error.localizedDescription);
}

- (void)application:(UIApplication*)application
    didReceiveRemoteNotification:(NSDictionary*)userInfo
          fetchCompletionHandler:(void (^)(UIBackgroundFetchResult))completionHandler {
  (void)application;
  NSString* type = nil;
  id raw = userInfo[@"type"];
  if ([raw isKindOfClass:[NSString class]]) {
    type = (NSString*)raw;
  }
  if ([type isEqualToString:@"call_wake"]) {
    pbr::BackgroundSyncScheduler::Instance().RequestCallWakeSync();
  } else {
    // inbox_wake or unknown opaque wake — force inbox sync.
    pbr::BackgroundSyncScheduler::Instance().RequestWakeSync();
  }
  if (completionHandler) {
    completionHandler(UIBackgroundFetchResultNewData);
  }
}

// In the foreground: the app rings and shows messages itself, so the alert is not shown; a call
// alert still makes it look for the ring now.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
  (void)center;
  if (IsCallWake(notification.request.content.userInfo)) {
    pbr::BackgroundSyncScheduler::Instance().RequestCallWakeSync();
  }
  completionHandler(UNNotificationPresentationOptionNone);
}

// A tap: a call alert brings the app up to ring; a message banner opens its chat.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler {
  (void)center;
  NSDictionary* userInfo = response.notification.request.content.userInfo;
  if (IsCallWake(userInfo)) {
    pbr::BackgroundSyncScheduler::Instance().RequestCallWakeSync();
  } else {
    id thread = userInfo[[NSString stringWithUTF8String:pbr::kIosNotificationThreadKey]];
    if ([thread isKindOfClass:[NSString class]] && [(NSString*)thread length] > 0) {
      pbr::DispatchIosNotificationActivation(std::string([(NSString*)thread UTF8String]));
    }
  }
  completionHandler();
}

@end

// SDL documents overriding +getAppDelegateClassName from a category; clang
// warns because SDLUIKitDelegate implements it too, which is the whole point
// (Xcode 27 clang, -Werror).
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation SDLUIKitDelegate (PpApnsExtensions)
+ (NSString*)getAppDelegateClassName {
  return @"PpSdlUIKitDelegate";
}
@end
#pragma clang diagnostic pop

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
