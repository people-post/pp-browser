#pragma once

#include "foundation/platform/ILocalNotifier.h"
#include "common/PbrCompat.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace pbr {

/**
 * iOS banners (UNUserNotificationCenter), posted while the app is not in the foreground — after a
 * push woke it (P010). The notification-center delegate (IosApnsAppDelegate) routes taps back here.
 */
class IosLocalNotifier final : public ILocalNotifier {
public:
  void NotifyIncoming(const std::string& title, const std::string& body, const std::string& thread_id = {},
                      int thread_unread = 0) override;
  void ClearForThread(const std::string& thread_id) override;
  void SetActivationHandler(std::function<void(std::string thread_id)> handler) override;
};

/**
 * Request identifier of the banners for `thread_id`. An incoming call (no thread) shares
 * `kIosCallNotificationId` with the relay's call alert (its `apns-collapse-id`), so the two replace
 * each other rather than stack.
 */
std::string IosNotificationIdForThread(const std::string& thread_id);
inline constexpr const char* kIosCallNotificationId = "pp-call";
/** userInfo key carrying a banner's thread id. */
inline constexpr const char* kIosNotificationThreadKey = "thread_id";
/** Top-level key of the relay's push carrying the call id / message id (`wake_hint`). */
inline constexpr const char* kIosNotificationWakeHintKey = "wake_hint";

/**
 * The user tapped a banner of `thread_id`, or a push alert (a call's with `call_wake`; empty thread; `wake_hint`:
 * the push's call id / message id). Always recorded in PendingNotificationTap; a thread is also handed to the
 * activation handler on the UI thread when one is set.
 */
void DispatchIosNotificationActivation(const std::string& thread_id, bool call_wake = false,
                                       const std::string& wake_hint = {},
                                       std::optional<std::chrono::milliseconds> notification_age = std::nullopt);

} // namespace pbr
