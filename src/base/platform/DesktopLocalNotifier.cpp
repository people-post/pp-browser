#include "base/platform/DesktopLocalNotifier.h"

#include "base/platform/AppLifecycle.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/desktop/LocalNotifierImpl.h"
#include "common/Logger.h"

#include <cstdio>

namespace pbr {

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
        if (!handler) {
          logging::getLogger("LocalNotifier").warning << "Activation ignored: no handler";
          return;
        }
        BrowserThread::PostTask(BrowserThreadId::UI, [handler, thread_id]() mutable {
          logging::getLogger("LocalNotifier").warning
              << "Activation on UI thread thread=" << thread_id;
          std::fprintf(stderr, "[Frame][LocalNotifier] Activation on UI thread thread=%s\n",
                       thread_id.c_str());
          std::fflush(stderr);
          handler(thread_id);
        });
      });
}

void DesktopLocalNotifier::Shutdown() {
  desktop::ShutdownDesktopNotifications();
}

} // namespace pbr
