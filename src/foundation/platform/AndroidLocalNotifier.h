#pragma once

#include "foundation/platform/ILocalNotifier.h"

namespace pbr {

/** Android NotificationCompat bridge (implemented via JNI helpers). */
class AndroidLocalNotifier final : public ILocalNotifier {
public:
  void NotifyIncoming(const std::string& title, const std::string& body,
                      const std::string& thread_id = {}) override;
  void ClearForThread(const std::string& thread_id) override;
};

} // namespace pbr
