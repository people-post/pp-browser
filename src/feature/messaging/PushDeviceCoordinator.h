#pragma once

#include "common/Error.h"

namespace pbr {

/** Register/unregister FCM tokens with Brief based on Show notifications pref. */
class PushDeviceCoordinator {
public:
  static Roe<void> SyncWithPreference(bool show_notifications);
  static Roe<void> UnregisterCurrent();
};

} // namespace pbr
