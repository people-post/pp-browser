#pragma once

#include "foundation/platform/ILocalNotifier.h"
#include "common/PbrCompat.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * iOS banners (UNUserNotificationCenter), posted while the app is not in the foreground — after a
 * push woke it (P010). The notification-center delegate (IosApnsAppDelegate) routes taps back here.
 */
class IosLocalNotifier final : public ILocalNotifier {
public:
  void NotifyIncoming(const std::string& title, const std::string& body,
                      const std::string& thread_id = {}) override;
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

/** The user tapped a banner of `thread_id` (any thread): runs the activation handler on the UI thread. */
void DispatchIosNotificationActivation(const std::string& thread_id);

} // namespace pbr
