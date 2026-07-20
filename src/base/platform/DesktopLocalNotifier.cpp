#include "base/platform/DesktopLocalNotifier.h"

#include "base/platform/AppLifecycle.h"
#include "base/platform/desktop/LocalNotifierImpl.h"

namespace pbr {

void DesktopLocalNotifier::NotifyIncoming(const std::string& title, const std::string& body,
                                          const std::string& /*thread_id*/) {
  if (AppLifecycle::IsForeground()) {
    return;
  }
  desktop::PostDesktopNotification(title.empty() ? "New message" : title,
                                   body.empty() ? "You have a new message" : body);
}

void DesktopLocalNotifier::ClearForThread(const std::string& /*thread_id*/) {}

} // namespace pbr
