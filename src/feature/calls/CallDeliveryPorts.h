#pragma once

#include "domain/messaging/SendRelayOptions.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Delivery ops CallSessionManager needs from conversations (consumer-declared).
 * Softens calls→conversations: calls must not include MeshMessagingService.
 */
struct CallDeliveryPorts {
  std::function<Roe<ThreadMessage>(const std::string& thread_id, const std::string& text,
                                   const SendRelayOptions& options)>
      send_user_message;
  std::function<void(bool force)> sync_inbox_from_wake;
  /** Register a dialable multiaddr for a peer identity (call listen bootstrap). */
  std::function<void(const std::string& identity, const std::string& multiaddr)> register_peer_direct_endpoint;

  bool IsBound() const {
    return static_cast<bool>(send_user_message) && static_cast<bool>(sync_inbox_from_wake);
  }
};

} // namespace pbr
