#pragma once

#include <functional>
#include <string>

namespace pbr::desktop {

using NotificationActivationFn = std::function<void(const std::string& thread_id)>;

/** Install handler invoked when the user activates an OS notification (may be off UI thread). */
void SetDesktopNotificationActivationHandler(NotificationActivationFn handler);

/**
 * Called by OS backends when a notification is activated.
 * `thread_id` may be empty (still raises the app).
 */
void DispatchDesktopNotificationActivation(const std::string& thread_id);

/** Optional compositor activation token from the notification server (Linux/GNOME). */
void SetPendingDesktopActivationToken(std::string token);
/** Takes the pending token if still fresh; empty if none. */
std::string TakePendingDesktopActivationToken();

void PostDesktopNotification(const std::string& title, const std::string& body,
                             const std::string& thread_id);
void ClearDesktopNotification(const std::string& thread_id);
void ShutdownDesktopNotifications();

} // namespace pbr::desktop
