#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"
#include "lib/net/IPushDeviceClient.h"

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

  /**
   * Binds `reg`, and beside it the iPhone's PushKit token as platform `ios_voip`, device id `<device_id>.voip`:
   * bound when `voip_wanted` (default PIN), removed otherwise. No token (`voip_token` empty): no VoIP binding is
   * touched. The first failure is returned; both are tried.
   */
  static Roe<void> RegisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                const std::string& voip_token, bool voip_wanted);
  /** Removes `reg` and, when there is a PushKit token, its VoIP binding. */
  static Roe<void> UnregisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                  const std::string& voip_token);
};

} // namespace pbr
