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
  /** Removes this device's bindings from the running backend (a failed one leaves them there). */
  static Roe<void> Unregister(ConversationsHub& hub);
  /** A saved backend switch (applied on restart) removes the binding now; undoing it registers again. */
  static void OnBackendSwitchPending(ConversationsHub& hub, bool pending);
};

} // namespace pbr
