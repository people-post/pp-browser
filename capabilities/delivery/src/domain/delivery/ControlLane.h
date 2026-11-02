#pragma once

#include "domain/peers/OwnCards.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/ThreadTypes.h"
#include "common/chat/IDirectMessageClient.h"

#include <atomic>
#include <functional>
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

/**
 * The control lane: identified system messages to a peer (call signaling and the like) — E2E under
 * the pair's e2e_public session (opened on first use), not sequenced into a thread, never stored.
 * Sent on the critical lane, the relay first when a route is known (retried while fresh), else Amp;
 * over a live link to the peer as well, at once (#431).
 * Any thread; the order keys are unique and rising per sender.
 */
class ControlLane : public Module {
public:
  ControlLane(IdentityStore& identity, OwnCards& own_keys, ContactsStore& contacts, PeerRouteBook& routes,
              E2ePublicSessionKeys& keys, std::function<IDirectMessageClient*()> direct_chat, DeliveryOutbox& outbox);

  /**
   * Send `control` to `peer_identity`. `key_init_b64`: a session init the caller holds (its media-key
   * wrap opened the session) rides the first send. `urgent`: wake the peer at once, the platform's most urgent push (an incoming call's).
   */
  Roe<void> Send(const std::string& peer_identity, const ThreadMessage& control,
                 const std::optional<std::string>& key_init_b64 = std::nullopt, bool urgent = false);

  /**
   * Tell this account's other devices (through its own mailbox, M022) our account card — the device list —
   * riding `notice`, a system message they take nothing else from.
   */
  Roe<void> NotifySiblings(const ThreadMessage& notice);

private:
  uint64_t NextOrderKey();

  IdentityStore& identity_;
  OwnCards& own_keys_;
  ContactsStore& contacts_;
  PeerRouteBook& routes_;
  E2ePublicSessionKeys& keys_;
  /** The mesh's direct chat client while Amp is attached; null otherwise. */
  std::function<IDirectMessageClient*()> direct_chat_;
  DeliveryOutbox& outbox_;
  std::atomic<uint64_t> order_key_{0};
};

} // namespace pbr
