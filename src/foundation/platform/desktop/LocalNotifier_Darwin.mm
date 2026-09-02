#if defined(__APPLE__)

#include "foundation/platform/desktop/LocalNotifierImpl.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <TargetConditionals.h>

#if !TARGET_OS_IPHONE

#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

#include <mutex>
#include <string>
#include <unordered_map>

static NSString* const kFrameThreadIdKey = @"thread_id";
static NSString* const kFrameCategoryId = @"frame.incoming_message";
static constexpr const char* kFrameIdPrefix = "frame-msg-";

@interface FrameNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation FrameNotificationDelegate

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
    API_AVAILABLE(macos(10.14)) {
  (void)center;
  (void)notification;
  if (@available(macOS 11, *)) {
    // Banner/List replaced Alert (deprecated on macOS 11+).
    completionHandler(UNNotificationPresentationOptionList | UNNotificationPresentationOptionBanner |
                      UNNotificationPresentationOptionSound);
  } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    completionHandler(UNNotificationPresentationOptionAlert | UNNotificationPresentationOptionSound);
#pragma clang diagnostic pop
  }
}

- (void)userNotificationCenter:(UNUserNotificationCenter*)center
    didReceiveNotificationResponse:(UNNotificationResponse*)response
             withCompletionHandler:(void (^)(void))completionHandler API_AVAILABLE(macos(10.14)) {
  (void)center;
  NSDictionary* userInfo = response.notification.request.content.userInfo;
  NSString* thread = userInfo[kFrameThreadIdKey];
  if (thread.length > 0) {
    pbr::desktop::DispatchDesktopNotificationActivation(std::string([thread UTF8String]));
  }
  completionHandler();
}

@end

namespace pbr::desktop {
namespace {

std::mutex g_mu;
bool g_init_attempted = false;
bool g_init_ok = false;
bool g_logged_fail = false;
std::unordered_map<std::string, std::string> g_thread_to_identifier;
FrameNotificationDelegate* g_delegate = nil;

void LogFailOnce(const char* detail) {
  if (g_logged_fail) {
    return;
  }
  g_logged_fail = true;
  logging::getLogger("LocalNotifier").warning
      << "Desktop notifications unavailable: " << (detail ? detail : "unknown");
}

std::string IdentifierForThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return std::string(kFrameIdPrefix) + "default";
  }
  return std::string(kFrameIdPrefix) + thread_id;
}

bool IsAppBundle() {
  CFBundleRef bundle = CFBundleGetMainBundle();
  if (!bundle) {
    return false;
  }
  CFURLRef bundle_url = CFBundleCopyBundleURL(bundle);
  if (!bundle_url) {
    return false;
  }
  // .app path is enough for UNUserNotificationCenter; avoids deprecated UTI helpers.
  NSURL* url = CFBridgingRelease(bundle_url);
  return [[url pathExtension] caseInsensitiveCompare:@"app"] == NSOrderedSame;
}

bool EnsureInitLocked() {
  if (g_init_attempted) {
    return g_init_ok;
  }
  g_init_attempted = true;

  if (!IsAppBundle()) {
    LogFailOnce("macOS notifications require an application bundle");
    return false;
  }
  if (@available(macOS 10.14, *)) {
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    if (!g_delegate) {
      g_delegate = [FrameNotificationDelegate new];
      [center setDelegate:g_delegate];
    }
    UNNotificationCategory* category =
        [UNNotificationCategory categoryWithIdentifier:kFrameCategoryId
                                               actions:@[]
                                     intentIdentifiers:@[]
                                               options:UNNotificationCategoryOptionNone];
    [center setNotificationCategories:[NSSet setWithObject:category]];
    [center getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
      if (settings.authorizationStatus == UNAuthorizationStatusNotDetermined) {
        UNAuthorizationOptions options =
            UNAuthorizationOptionAlert | UNAuthorizationOptionSound | UNAuthorizationOptionBadge;
        [center requestAuthorizationWithOptions:options
                              completionHandler:^(BOOL granted, NSError* error) {
                                if (!granted) {
                                  const char* detail =
                                      error ? [[error localizedDescription] UTF8String]
                                            : "Notification permission denied";
                                  logging::getLogger("LocalNotifier").warning << detail;
                                }
                              }];
      }
    }];
    g_init_ok = true;
    return true;
  }
  LogFailOnce("Notifications require macOS 10.14+");
  return false;
}

} // namespace

void PostDesktopNotification(const std::string& title, const std::string& body,
                             const std::string& thread_id) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!EnsureInitLocked()) {
      return;
    }
    if (@available(macOS 10.14, *)) {
      const std::string identifier = IdentifierForThread(thread_id);
      UNMutableNotificationContent* content = [UNMutableNotificationContent new];
      content.title = [NSString stringWithUTF8String:title.c_str()];
      content.body = [NSString stringWithUTF8String:body.c_str()];
      content.sound = [UNNotificationSound defaultSound];
      content.categoryIdentifier = kFrameCategoryId;
      if (!thread_id.empty()) {
        content.userInfo = @{kFrameThreadIdKey : [NSString stringWithUTF8String:thread_id.c_str()]};
      }

      UNNotificationRequest* request =
          [UNNotificationRequest requestWithIdentifier:[NSString stringWithUTF8String:identifier.c_str()]
                                               content:content
                                               trigger:nil];
      [[UNUserNotificationCenter currentNotificationCenter]
          addNotificationRequest:request
           withCompletionHandler:^(NSError* error) {
             if (error) {
               logging::getLogger("LocalNotifier").warning
                   << "Failed to post notification: "
                   << [[error localizedDescription] UTF8String];
             }
           }];
      if (!thread_id.empty()) {
        g_thread_to_identifier[thread_id] = identifier;
      }
    }
  }
}

void ClearDesktopNotification(const std::string& thread_id) {
  @autoreleasepool {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_init_ok || thread_id.empty()) {
      return;
    }
    if (@available(macOS 10.14, *)) {
      std::string identifier;
      const auto it = g_thread_to_identifier.find(thread_id);
      if (it != g_thread_to_identifier.end()) {
        identifier = it->second;
        g_thread_to_identifier.erase(it);
      } else {
        identifier = IdentifierForThread(thread_id);
      }
      NSString* ns_id = [NSString stringWithUTF8String:identifier.c_str()];
      UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
      [center removeDeliveredNotificationsWithIdentifiers:@[ ns_id ]];
      [center removePendingNotificationRequestsWithIdentifiers:@[ ns_id ]];
    }
  }
}

void ShutdownDesktopNotifications() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_thread_to_identifier.clear();
  g_init_attempted = false;
  g_init_ok = false;
}

} // namespace pbr::desktop

#else // TARGET_OS_IPHONE

namespace pbr::desktop {

void PostDesktopNotification(const std::string& /*title*/, const std::string& /*body*/,
                             const std::string& /*thread_id*/) {}

void ClearDesktopNotification(const std::string& /*thread_id*/) {}

void ShutdownDesktopNotifications() {}

} // namespace pbr::desktop

#endif // !TARGET_OS_IPHONE

#endif // __APPLE__
