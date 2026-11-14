#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

class ConversationsHub;

/**
 * Registers this device's push token with our relays. Always, whatever "Show notifications" says:
 * in the background a phone is reached by push only (P010), and calls ring with alerts off too; the
 * setting hides message banners only (P005). Account revoke deletes the relay's device rows.
 */
class PushDeviceCoordinator {
public:
  /** On the preferred relay and on each other relay of ours that holds our registration (multi-relay R2). */
  static Roe<void> Register(ConversationsHub& hub);
  /** On one other relay of ours, once it took our registration (RelayRegistrationsWorkflow). */
  static Roe<void> RegisterOn(ConversationsHub& hub, const std::string& origin);
  /** Removes this device's bindings from the running backend (a failed one leaves them there). */
  static Roe<void> Unregister(ConversationsHub& hub);
  /** A saved backend switch (applied on restart) removes the binding now; undoing it registers again. */
  static void OnBackendSwitchPending(ConversationsHub& hub, bool pending);
};

} // namespace pbr
