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
