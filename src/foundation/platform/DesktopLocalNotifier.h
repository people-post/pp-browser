#pragma once

#include "foundation/platform/ILocalNotifier.h"
#include "common/Module.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class DesktopLocalNotifier final : public ILocalNotifier, public Module {
public:
  DesktopLocalNotifier();

  void NotifyIncoming(const std::string& title, const std::string& body,
                      const std::string& thread_id = {}) override;
  void ClearForThread(const std::string& thread_id) override;
  void SetActivationHandler(std::function<void(std::string thread_id)> handler) override;
  void Shutdown() override;
};

} // namespace pbr
