#pragma once

#include "domain/peers/OwnCards.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/ThreadTypes.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class ContactsStore;
class DeliveryOutbox;
class E2ePublicSessionKeys;
class IdentityStore;
class PeerRouteBook;
class PeerTransports;

/**
 * The control lane: identified system messages to a peer (call signaling and the like) — E2E under
 * the pair's e2e_public session (opened on first use), not sequenced into a thread, never stored.
 * Sent on the critical lane, the relay first when a route is known (retried while fresh), else Amp.
 * Any thread; the order keys are unique and rising per sender.
 */
class ControlLane : public Module {
public:
  ControlLane(IdentityStore& identity, OwnCards& own_keys, ContactsStore& contacts, PeerRouteBook& routes,
              E2ePublicSessionKeys& keys, PeerTransports& transports, DeliveryOutbox& outbox);

  /**
   * Send `control` to `peer_identity`. `key_init_b64`: a session init the caller holds (its media-key
   * wrap opened the session) rides the first send. `urgent`: wake the peer as for an incoming call.
   */
  Roe<void> Send(const std::string& peer_identity, const ThreadMessage& control,
                 const std::optional<std::string>& key_init_b64 = std::nullopt, bool urgent = false);

private:
  uint64_t NextOrderKey();

  IdentityStore& identity_;
  OwnCards& own_keys_;
  ContactsStore& contacts_;
  PeerRouteBook& routes_;
  E2ePublicSessionKeys& keys_;
  PeerTransports& transports_;
  DeliveryOutbox& outbox_;
  std::atomic<uint64_t> order_key_{0};
};

} // namespace pbr
