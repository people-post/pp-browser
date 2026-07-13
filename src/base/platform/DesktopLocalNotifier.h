#pragma once

#include "base/platform/ILocalNotifier.h"

namespace pbr {

class DesktopLocalNotifier final : public ILocalNotifier {
public:
  void NotifyIncoming(const std::string& title, const std::string& body,
                      const std::string& thread_id = {}) override;
  void ClearForThread(const std::string& thread_id) override;
};

} // namespace pbr
