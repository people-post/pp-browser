#include "foundation/platform/IosLocalNotifier.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "foundation/runtime/AppLifecycle.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/Logger.h"

#import <UserNotifications/UserNotifications.h>

#include <mutex>
#include <utility>

namespace pbr {

namespace {

std::mutex g_mu;
std::function<void(std::string thread_id)> g_activation;

NSString* ToNs(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()];
}

} // namespace

std::string IosNotificationIdForThread(const std::string& thread_id) {
  return thread_id.empty() ? std::string(kIosCallNotificationId) : "pp-msg-" + thread_id;
}

void IosLocalNotifier::NotifyIncoming(const std::string& title, const std::string& body,
                                      const std::string& thread_id) {
  // In the foreground the app shows it itself (and rings).
  if (AppLifecycle::IsForeground()) {
    return;
  }
  @autoreleasepool {
    UNMutableNotificationContent* content = [[UNMutableNotificationContent new] autorelease]; // MRC
    content.title = ToNs(title.empty() ? Tr("notification.new_message") : title);
    content.body = ToNs(body.empty() ? Tr("notification.new_message_body") : body);
    content.sound = [UNNotificationSound defaultSound];
    if (!thread_id.empty()) {
      content.threadIdentifier = ToNs(thread_id);
      content.userInfo = @{ToNs(kIosNotificationThreadKey) : ToNs(thread_id)};
    }
    if (thread_id.empty()) {
      // An incoming call, like the relay's call alert (P012).
      content.interruptionLevel = UNNotificationInterruptionLevelTimeSensitive;
    }
    UNNotificationRequest* request =
        [UNNotificationRequest requestWithIdentifier:ToNs(IosNotificationIdForThread(thread_id))
                                             content:content
                                             trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:request
         withCompletionHandler:^(NSError* error) {
           if (error) {
             logging::getLogger("LocalNotifier").warning
                 << "Failed to post notification: " << [[error localizedDescription] UTF8String];
           }
         }];
  }
}

void IosLocalNotifier::ClearForThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return;
  }
  @autoreleasepool {
    NSString* identifier = ToNs(IosNotificationIdForThread(thread_id));
    UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
    [center removeDeliveredNotificationsWithIdentifiers:@[ identifier ]];
    [center removePendingNotificationRequestsWithIdentifiers:@[ identifier ]];
  }
}

void IosLocalNotifier::SetActivationHandler(std::function<void(std::string thread_id)> handler) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_activation = std::move(handler);
}

void DispatchIosNotificationActivation(const std::string& thread_id, const bool call_wake,
                                       const std::string& wake_hint) {
  // Recorded first: with no handler yet (locked / cold start) WireMessagingBindings picks it up.
  PendingNotificationTap::Instance().Record(thread_id, call_wake, wake_hint, PendingNotificationTap::Clock::now());
  if (thread_id.empty()) {
    return;
  }
  std::function<void(std::string)> handler;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    handler = g_activation;
  }
  if (!handler) {
    return;
  }
  AppRuntime::PostUI([handler = std::move(handler)]() mutable {
    if (auto pending = PendingNotificationTap::Instance().TakeThread()) {
      handler(std::move(*pending));
    }
  });
}

} // namespace pbr
