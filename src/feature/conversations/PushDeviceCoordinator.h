#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsHub;

/** Register/unregister FCM tokens with Brief based on Show notifications pref. */
class PushDeviceCoordinator {
public:
  static Roe<void> SyncWithPreference(ConversationsHub& hub, bool show_notifications);
  static Roe<void> UnregisterCurrent(ConversationsHub& hub);
};

} // namespace pbr
