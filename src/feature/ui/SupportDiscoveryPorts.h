#pragma once

#include "base/net/ClientCompat.h"

#include <functional>
#include <optional>

namespace pbr {

/**
 * ClientCompat → Application: publish app Support discovery (product help desk Account)
 * after each resolved document. Clear via BindSupportDiscovery({}).
 */
struct SupportDiscoveryPorts {
  std::function<void(const std::optional<ClientCompatSupport>& support)> on_support_changed;
};

} // namespace pbr
