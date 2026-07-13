#pragma once

#include <string>

namespace pbr {

/** OS-level incoming-message banners (not in-app ShellFeedback). */
class ILocalNotifier {
public:
  virtual ~ILocalNotifier() = default;

  virtual void NotifyIncoming(const std::string& title, const std::string& body,
                              const std::string& thread_id = {}) = 0;
  virtual void ClearForThread(const std::string& thread_id) = 0;

  static ILocalNotifier& Instance();
  static void SetInstance(ILocalNotifier* notifier);
};

} // namespace pbr
