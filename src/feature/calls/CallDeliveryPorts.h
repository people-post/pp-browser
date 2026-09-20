#pragma once

#include "domain/messaging/SendRelayOptions.h"
#include "domain/messaging/E2ePublicSessionLogic.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <functional>
#include <string>

namespace pbr {

/**
 * Delivery ops CallSessionManager needs from conversations (consumer-declared).
 * Softens calls→conversations: calls must not include MeshDeliveryOrchestrator.
 */
struct CallDeliveryPorts {
  std::function<pp::Roe<ThreadMessage>(const std::string& thread_id, const std::string& text,
                                       const SendRelayOptions& options)>
      send_user_message;
  std::function<void(bool force)> sync_inbox_from_wake;
  /** Register a dialable multiaddr for a peer identity (call listen bootstrap). */
  std::function<void(const std::string& identity, const std::string& multiaddr)> register_peer_direct_endpoint;
  /**
   * Ensure e2e_public session key for media-key wrap (AutoKey if missing).
   * When `first_message_key_init_b64` is set, attach it on the next call-control send.
   */
  std::function<pp::Roe<EnsuredE2ePublicSessionKey>(const std::string& peer_identity)> ensure_peer_session_key;
  /**
   * After the last local call ends — flush inbox + TailSync that were deferred while
   * HasActiveLocalCall. No-op while another call is still ActiveLocalCall.
   */
  std::function<void()> catch_up_after_call;

  bool IsBound() const {
    return static_cast<bool>(send_user_message) && static_cast<bool>(sync_inbox_from_wake);
  }
};

} // namespace pbr
