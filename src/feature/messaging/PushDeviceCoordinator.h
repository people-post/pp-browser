#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

class MessagingHub;

/** Register/unregister FCM tokens with Brief based on Show notifications pref. */
class PushDeviceCoordinator {
public:
  static Roe<void> SyncWithPreference(MessagingHub& hub, bool show_notifications);
  static Roe<void> UnregisterCurrent(MessagingHub& hub);
};

} // namespace pbr
