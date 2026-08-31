#include "base/platform/DesktopLocalNotifier.h"

#include "base/runtime/AppLifecycle.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/desktop/LocalNotifierImpl.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"

namespace pbr {

DesktopLocalNotifier::DesktopLocalNotifier() {
  redirectLogger("LocalNotifier");
}

void DesktopLocalNotifier::NotifyIncoming(const std::string& title, const std::string& body,
                                          const std::string& thread_id) {
  if (AppLifecycle::IsUserAttentive()) {
    return;
  }
  desktop::PostDesktopNotification(title.empty() ? "New message" : title,
                                   body.empty() ? "You have a new message" : body, thread_id);
}

void DesktopLocalNotifier::ClearForThread(const std::string& thread_id) {
  desktop::ClearDesktopNotification(thread_id);
}

void DesktopLocalNotifier::SetActivationHandler(
    std::function<void(std::string thread_id)> handler) {
  desktop::SetDesktopNotificationActivationHandler(
      [handler = std::move(handler)](const std::string& thread_id) {
        // Named logger (same node as Module redirect) — safe from watch thread.
        if (!handler) {
          logging::getLogger("LocalNotifier").warning << "Activation ignored: no handler";
          return;
        }
        AppRuntime::PostUI([handler, thread_id]() mutable {
          logging::getLogger("LocalNotifier").info
              << "Activation on UI thread thread=" << thread_id;
          handler(thread_id);
        });
      });
}

void DesktopLocalNotifier::Shutdown() {
  desktop::ShutdownDesktopNotifications();
}

} // namespace pbr
