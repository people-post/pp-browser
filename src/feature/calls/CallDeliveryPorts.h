#pragma once

#include "lib/messaging/SendRelayOptions.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <set>
#include <string>

namespace pbr {

/**
 * Delivery ops CallSessionManager needs from conversations (consumer-declared).
 * Softens calls→conversations: calls must not include MeshDeliveryOrchestrator.
 */
struct CallDeliveryPorts {
  /**
   * Call signaling to a peer on the control lane (no thread, never a transcript row). `key_init_b64`:
   * the AutoKey init of a session a media-key wrap just opened, for the peer to derive it. `urgent`:
   * a call invite — the relay wakes the peer as for an incoming call (P012).
   */
  std::function<pp::Roe<void>(const std::string& peer_identity, const ThreadMessage& control,
                              const std::optional<std::string>& key_init_b64, bool urgent)>
      send_control;
  /**
   * A control notice to our own account's other devices (M021 d5: a call answered here stops ringing there);
   * nothing when this device is the account's only one. Unset: not told.
   */
  std::function<pp::Roe<void>(const ThreadMessage& control)> notify_own_devices;
  /** A message into a thread's chat stream (direct or group): the call summary. */
  std::function<pp::Roe<ThreadMessage>(const std::string& thread_id, const std::string& text,
                                       const SendRelayOptions& options)>
      send_to_thread;
  /** This device's DM with a peer: the private E2E one when there is one, else the public (made if missing). */
  std::function<pp::Roe<std::string>(const std::string& peer_identity)> resolve_peer_thread;
  /** This device's thread for a group; none when it has none. */
  std::function<std::optional<std::string>(const std::string& group_id)> resolve_group_thread;
  /** Who is in a group now (identities); none when this device does not know the group. */
  std::function<std::optional<std::set<std::string>>(const std::string& group_id)> group_members;
  std::function<void(bool force)> sync_inbox_from_wake;
  /** Register a dialable multiaddr for a peer identity (call listen bootstrap). */
  std::function<void(const std::string& identity, const std::string& multiaddr)> register_peer_direct_endpoint;
  /**
   * Ensure e2e_public session key for media-key wrap (AutoKey if missing).
   * When `first_message_key_init_b64` is set, it rides the next control-lane send to that peer.
   */
  std::function<pp::Roe<EnsuredE2ePublicSessionKey>(const std::string& peer_identity)> ensure_peer_session_key;
  /**
   * After the last local call ends — flush inbox + TailSync that were deferred while
   * HasActiveLocalCall. No-op while another call is still ActiveLocalCall.
   */
  std::function<void()> catch_up_after_call;

  bool IsBound() const {
    return static_cast<bool>(send_control) && static_cast<bool>(sync_inbox_from_wake);
  }
};

} // namespace pbr
