#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"
#include "lib/net/IPushDeviceClient.h"

#include <string>

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

  /**
   * Binds `reg`, and beside it the VoIP token of a CallKit iPhone (`voip_token` non-empty, P016) as platform
   * `ios_voip`, device id `<reg.device_id>.voip`. The first failure is returned; both are tried.
   */
  static Roe<void> RegisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                const std::string& voip_token);
  static Roe<void> UnregisterWith(IPushDeviceClient& client, const PushDeviceRegistration& reg,
                                  const std::string& voip_token);
};

} // namespace pbr
