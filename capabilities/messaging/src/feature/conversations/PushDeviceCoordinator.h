#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsHub;

/**
 * Registers this device's push token with the relay. Always, whatever "Show notifications" says:
 * in the background a phone is reached by push only (P010), and calls ring with alerts off too; the
 * setting hides message banners only (P005). Account revoke deletes the relay's device rows.
 */
class PushDeviceCoordinator {
public:
  static Roe<void> Register(ConversationsHub& hub);
};

} // namespace pbr
