#pragma once

#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/delivery/ControlSend.h"
#include "common/Lifetime.h"
#include "common/WorkerPool.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/ThreadTypes.h"
#include "common/chat/IDirectMessageClient.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ContactsStore;
class DeliveryOutbox;
class E2ePublicSessionKeys;
class IdentityStore;
class PeerRouteBook;

/** What a notice to this account's other devices is: what it carries and how long the relay keeps it. */
enum class SiblingNotice {
  /** The device list: our card rides it — the card is the notice. Kept the relay's full window. */
  DeviceList,
  /** A sent message's copy (M021 d3): history for a sibling that may be off for days. Kept the full window. */
  SentCopy,
  /** A family's lane signaling (a call taken here): fresh only, so kept kControlLaneRelayTtlMs. */
  Signal,
};

/**
 * The control lane: identified system messages to a peer (call signaling and the like) — not sequenced into a
 * thread, never stored. One relay record with a copy per device on the peer's newest card, each E2E under that
 * device pair's e2e_public session (the chat stream's, opened on first use; M024); the account copy, under the
 * account pair's, only for a peer we hold no card for (and, for one release, between two single-device accounts).
 * Sent on the critical lane, the relay first when a route is known (retried while fresh), else Amp; over a live
 * link to the peer as well, at once (#431). A send to many seals each peer's record in parallel off the caller's
 * thread (V058 2). Any thread; the order keys are unique and rising per sender.
 */
class ControlLane : public Module {
public:
  ControlLane(IdentityStore& identity, OwnCards& own_keys, AccountKeyResolver& peer_cards, ContactsStore& contacts,
              PeerRouteBook& routes, E2ePublicSessionKeys& keys, std::function<IDirectMessageClient*()> direct_chat,
              DeliveryOutbox& outbox);
  /** Waits out a send to many that is resolving; the rest of it is dropped. */
  ~ControlLane();

  /**
   * Send `control` to `peer_identity`. `key_init_b64`: an account-pair session init the caller holds (its old-form
   * media-key wrap opened it, M024) — it rides an account copy only, never a device's. `urgent`: wake the peer at
   * once, the platform's most urgent push (an incoming call's). Our account card rides it only when the peer may
   * not hold it (ControlLaneCarriesCard): `with_card` (or `urgent`) when the caller knows it may not. The relay
   * keeps it kControlLaneRelayTtlMs.
   */
  Roe<void> Send(const std::string& peer_identity, const ThreadMessage& control,
                 const std::optional<std::string>& key_init_b64 = std::nullopt, bool urgent = false,
                 bool with_card = false);

  /**
   * Each of `sends` to its peer, as Send — resolved (route, maybe a directory lookup; the pair's session, maybe
   * opened) and sealed on `lane`'s workers, in parallel, never on the caller's thread. `done` (optional) gets one
   * result per send, in their order, on a worker, once the last is queued or refused. Live signaling takes the
   * critical lane; a notice to many that can wait takes a lower one, not to crowd it.
   */
  void SendToMany(std::vector<ControlSend> sends, ControlSendsDone done = {}, WorkerLane lane = WorkerLane::Critical);

  /**
   * Tell this account's other devices `notice` through its own mailbox (M022). Our card rides a device list, and
   * any notice after the card changed; the relay keeps a signal kControlLaneRelayTtlMs, the rest its full window.
   */
  Roe<void> NotifySiblings(const ThreadMessage& notice, SiblingNotice kind);

private:
  uint64_t NextOrderKey();
  /**
   * The copies a send to `peer_identity` goes as (M024): one per device on its newest card we hold, each under that
   * device pair's session — or the account copy, under the account pair's, with `account_key_init` riding it when
   * the session has none of its own to announce. A device whose session cannot be had is left out (logged).
   */
  Roe<std::vector<ControlLaneCopy>> CopiesFor(const std::string& peer_identity,
                                              const std::optional<std::string>& account_key_init);
  /** The account copy to `peer_identity` (a first contact, the single-device bridge). */
  Roe<ControlLaneCopy> AccountCopyFor(const std::string& peer_identity,
                                      const std::optional<std::string>& account_key_init);
  /** Whether our card at `card_at` rides this send to `account` (ControlLaneCarriesCard); noted when it does. */
  bool CarryCard(const std::string& account, bool opens_session, bool asked, int64_t card_at);

  IdentityStore& identity_;
  OwnCards& own_keys_;
  /** The peers' kept cards: their device lists (M021). */
  AccountKeyResolver& peer_cards_;
  ContactsStore& contacts_;
  PeerRouteBook& routes_;
  E2ePublicSessionKeys& keys_;
  /** The mesh's direct chat client while Amp is attached; null otherwise. */
  std::function<IDirectMessageClient*()> direct_chat_;
  DeliveryOutbox& outbox_;
  std::atomic<uint64_t> order_key_{0};
  /** Per account (ours too): the version of our card this device last sent it, while it runs. */
  std::mutex cards_sent_mutex_;
  std::unordered_map<std::string, int64_t> cards_sent_;
  pp::Lifetime lifetime_;
};

} // namespace pbr
