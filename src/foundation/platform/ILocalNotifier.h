#pragma once

#include <functional>
#include <string>

namespace pbr {

/** OS-level incoming-message banners (not in-app ShellFeedback). */
class ILocalNotifier {
public:
  virtual ~ILocalNotifier() = default;

  virtual void NotifyIncoming(const std::string& title, const std::string& body,
                              const std::string& thread_id = {}) = 0;
  virtual void ClearForThread(const std::string& thread_id) = 0;

  /**
   * Me → Show notifications (any thread). Where the OS can wake the app's process without the app (an Android
   * push), the platform keeps it so a banner posted then follows the setting too. Default: nothing to keep.
   */
  virtual void SetMessageBannersOn(bool on) { (void)on; }

  /**
   * Optional: invoked on the UI thread when the user taps an OS notification.
   * Default is a no-op (platforms without tap routing).
   */
  virtual void SetActivationHandler(std::function<void(std::string thread_id)> handler) {
    (void)handler;
  }

  /** Tear down native watchers/threads. Safe to call more than once. */
  virtual void Shutdown() {}

  static ILocalNotifier& Instance();
  static void SetInstance(ILocalNotifier* notifier);
};

} // namespace pbr
