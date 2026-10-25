#pragma once

#include "domain/delivery/AmpAccountCardTransport.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Account cards between devices (docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp): serves this
 * account's card to the devices that ask (AnswerCardRequest decides who may have it), asks a device for
 * its account's card, and learns an account's devices from the cards they give — the card being the one
 * thing that says a device speaks for an account. Any thread.
 */
class AccountCardService : public Module {
public:
  AccountCardService(IdentityStore& identity, OwnCards& own, AccountKeyResolver& keys, ContactsStore& contacts);

  /** Our address audience is Everyone (then an unauthenticated asker gets our key card). */
  void SetAudienceEveryone(std::function<bool()> everyone) { audience_everyone_ = std::move(everyone); }
  /**
   * Make a device reachable when no link to it is up (a circuit through the seed it is parked on, then a
   * punch): `done(true)` once a link is up. Without it, only devices already linked are asked.
   */
  void SetReach(std::function<void(const std::string& peer_id, std::function<void(bool reached)> done)> reach) {
    reach_ = std::move(reach);
  }
  /** Serve and ask over `transport`; null while the mesh is down. */
  void Attach(AmpAccountCardTransport* transport);

  /** Ask device `peer_id` for `account_id`'s card; kept when it proves itself and lists that device. */
  void Fetch(const std::string& peer_id, const std::string& account_id,
             std::function<void(Roe<PeerCardRecord>)> on_done = {});
  /**
   * A message from `account_id`, verified, came over a link with `peer_id`: when its card does not list
   * that device yet, ask the device for it (once in a while per device).
   */
  void NoteDevice(const std::string& account_id, const std::string& peer_id);
  /**
   * Tell this account's other devices our card (M021): ask each for its card, ours riding the ask — each
   * takes the other's (OwnCards::Learn). Siblings not reached are tried again a few times.
   */
  void TellSiblings();
  /** The PeerIds of `account_id`'s devices this device knows (kept cards). */
  std::vector<std::string> DevicesOf(const std::string& account_id);
  /** What is kept for `account_id` (no lookup). */
  Roe<std::optional<PeerCardRecord>> Kept(const std::string& account_id) { return keys_.Kept(account_id); }

  /** How long a device that answered is not asked again. */
  static constexpr int64_t kAskAgainAfterMs = 10 * 60 * 1000;
  /** A device not reached yet (its link is still coming up) is tried again this often, this many times. */
  static constexpr int64_t kRetryAfterMs = 15 * 1000;
  static constexpr int kMaxTries = 3;

private:
  /** How a fetch ended: answered (a card, or a refusal / a card that did not hold), or not reached. */
  enum class Outcome { Answered, NotReached };
  /** Ask over each of `link_keys` in turn until one is reached; the card must list `peer_id`. */
  void FetchOver(std::vector<std::string> link_keys, const std::string& peer_id, const std::string& account_id,
                 std::function<void(Outcome, Roe<PeerCardRecord>)> on_done);
  AccountCardReply Answer(const AccountCardRequest& request, const std::string& remote_peer_id);
  /** One sibling, `attempt` of kMaxTries. */
  void TellSibling(const std::string& account_id, const std::string& peer_id, int attempt);
  /** A contact's device rows follow its account's card (so dials to them pass the address audience). */
  void AddDevicesToContact(const PeerCardRecord& record);

  IdentityStore& identity_;
  OwnCards& own_;
  AccountKeyResolver& keys_;
  ContactsStore& contacts_;
  std::function<bool()> audience_everyone_;
  std::function<void(const std::string&, std::function<void(bool)>)> reach_;
  /** FetchOver, and when nothing was reached, once more after making `peer_id` reachable. */
  void FetchReaching(const std::string& peer_id, const std::string& account_id,
                     std::function<void(Outcome, Roe<PeerCardRecord>)> on_done);
  std::mutex mutex_;
  AmpAccountCardTransport* transport_ = nullptr;
  struct Asking {
    int64_t next_allowed_ms = 0;
    int tries = 0;
  };
  std::unordered_map<std::string, Asking> asking_;
  /** Outlives this service in the retry timers' captures. */
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
