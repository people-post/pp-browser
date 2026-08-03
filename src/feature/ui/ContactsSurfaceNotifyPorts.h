#pragma once

#include "feature/ui/ContactsSurfaceSnapshot.h"

#include <functional>

namespace pbr {

/**
 * Contacts surface → composition-root notify. Application fills from ContactsShellBridge.
 * Clear via BindSurfaceNotify({}). Controller must not see shell chrome apply types.
 */
struct ContactsSurfaceNotifyPorts {
  std::function<void(const ContactsSurfaceSnapshot&)> push_surface;
};

} // namespace pbr
