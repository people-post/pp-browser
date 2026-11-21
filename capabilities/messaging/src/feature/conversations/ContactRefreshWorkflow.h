#pragma once

#include "domain/peers/AccountKeyResolver.h"
#include "feature/conversations/ContactCardExchange.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/PeerCards.h"
#include "lib/net/OrgBackendClientsImpl.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** What a contact refresh came to: which sources answered and what changed (the page says it in words). */
struct ContactRefreshResult {
  enum class Devices {
    NotAsked,   // no Account ID or no device to ask
    Answered,   // a device gave its account's card, and it checked out
    NotReached, // no device answered (offline, or what it gave did not hold)
  };
  /** One relay asked by Account ID, in the order asked (multi-relay R3). */
  struct RelayAsk {
    enum class Whose {
      Theirs, // one of the contact's relays (its card's list, else its QR locator's origin)
      Ours,   // one of our relays (or our other directories)
    };
    enum class Outcome {
      Answered,    // it keeps the account, and its answer verified: it was used
      NotFound,    // it answered, and does not know the account (not registered there)
      Unreachable, // it could not be asked (offline, a 5xx, a bad answer)
      Rejected,    // it answered, but not verifiably for this account: nothing of it was used, the next was asked
    };
    std::string origin;
    Whose whose = Whose::Theirs;
    Outcome outcome = Outcome::Unreachable;

    bool operator==(const RelayAsk&) const = default;
  };
  Devices devices = Devices::NotAsked;
  /** The relays asked, in order: the contact's in list order, then ours, up to the first whose answer verified. */
  std::vector<RelayAsk> relays;
  bool name_changed = false;
  bool photo_changed = false;
  bool keys_changed = false;
  bool devices_changed = false;
  /** Something else stored changed (a route, an address). */
  bool other_changed = false;
  /**
   * A relay's answer did not verify: it was for another account, or its keys (or its card) do not verify against the
   * contact's Account ID, or it carried neither. Nothing of it was used (Outcome::Rejected).
   */
  bool relay_keys_rejected = false;
  /**
   * A relay's answer named keys of an algorithm (or a KEM binding version) this build does not read (the row's
   * `keys_update_needed`): the contact needs a newer app here, not a forgery. Nothing of it was used (Rejected).
   */
  bool relay_keys_update_needed = false;
  /** The contact as stored after the refresh; empty when it is gone. */
  std::optional<Contact> contact;

  bool Changed() const { return name_changed || photo_changed || keys_changed || devices_changed || other_changed; }
  /** The relay whose answer was used; nullptr when none knew the account. */
  const RelayAsk* AnsweredBy() const;
  /** Neither the devices nor any relay answered. */
  bool NothingAnswered() const { return devices != Devices::Answered && AnsweredBy() == nullptr; }
};

/** The contact page's relay line (multi-relay R3): the contact's preferred relay, and whether it answered for it. */
struct ContactRelayView {
  enum class List {
    NotKnown,   // no card kept yet, and no locator gave a relay
    NoneListed, // its card lists no relay: our sends to it go through our preferred relay
    Listed,     // `origin` is its preferred relay
  };
  enum class Reach {
    NotKnown,         // not asked since the app started
    Reachable,        // at the last refresh it answered for the account, verifiably
    NotRegistered,    // at the last refresh it answered without the account (our copy of its list may be stale)
    Unreachable,      // at the last refresh it could not be asked, or what it answered did not verify
    IdentityMismatch, // the PeerId its card names is not the relay's by our pins, or the relay refused it (MR007)
  };
  List list = List::NotKnown;
  std::string origin;
  Reach reach = Reach::NotKnown;

  bool operator==(const ContactRelayView&) const = default;
};

/** What the sources gave, for MergeContactRefresh. */
struct ContactRefreshInputs {
  /** The account's kept card before and after the devices were asked (and the directory's keys kept). */
  std::optional<PeerCardRecord> card_before;
  std::optional<PeerCardRecord> card_after;
  /** A device answered now, or a relay's stored card was kept: the kept card's nickname is the newest word. */
  bool card_fresh = false;
  /** The answering relay's row; merged only when its answer verified (`hit_keys_verified`). */
  std::optional<mesh::DirectoryHit> hit;
  /**
   * The answer verified against the contact's Account ID (its keys, or the card it carried, were kept): only then is
   * the row used at all — its name, photo, addresses and Account ID. An unverified relay answer contributes nothing.
   */
  bool hit_keys_verified = false;
};

/**
 * Folds a refresh into `contact` and says what changed. Keys and devices are the account's verified card's
 * (the key stores hold the keys; the card's devices become device rows); the nickname is the kept card's when a
 * card came in now (from a device or a relay), else the relay row's; the photo and the device addresses are the
 * relay row's — a row only from an answer that verified (else none of it is used), whose Account ID may then be
 * adopted. The user's own fields (remark, trust) are never touched.
 */
Contact MergeContactRefresh(Contact contact, const ContactRefreshInputs& inputs, int64_t now_ms,
                            ContactRefreshResult& result);

/**
 * The contact page's Refresh (multi-relay R0, R3): asks, in order of trust, the contact's devices over the mesh for
 * its account's card (a forced ask, ContactCardExchange::AskDevices); then the contact's relays in list order
 * (ContactRelays: its kept card's, else its QR locator's origin), by Account ID (`GET /users/by-account`, public);
 * then our relays (SetOurRelays) not asked already — up to the first relay whose answer for the account verifies: it
 * is for that Account ID, and its row's keys (and its stored card, if it carries one) verify against it
 * (AccountKeyResolver: VerifyAccountCard, VerifyKeyCard). An answer that does not verify contributes nothing — no keys,
 * card, name, photo or addresses — and the next relay is asked (Outcome::Rejected). What each relay answered is
 * remembered for the contact page (PreferredRelay). The relay steps run on the blocking lane; the merge, the write and `done` run on the UI lane. One
 * run per contact at a time.
 */
class ContactRefreshWorkflow {
public:
  /**
   * One relay's row for an account, unauthenticated; none: not registered there (LookupStoredAccount). `public_only`:
   * one of the contact's relays, held to a public host outside the lab (DevCaBundleActive).
   */
  using RelayLookup = std::function<Roe<std::optional<StoredAccountRow>>(
      const std::string& origin, const std::string& account_id, bool public_only)>;
  /** Relays remembered at most for the contact page (the oldest forgotten first). */
  static constexpr size_t kMaxRemembered = 512;

  ContactRefreshWorkflow(ContactsStore& contacts, AccountKeyResolver& keys, ContactCardExchange& cards);
  ~ContactRefreshWorkflow();

  using Post = std::function<void(std::function<void()>)>;
  /** Where the relay lookups run (a worker) and where the result lands (the UI thread); tests queue them. */
  void SetPosts(Post blocking, Post ui);
  /** Tests: how a relay is asked (default: over HTTP). */
  void SetLookup(RelayLookup lookup);
  /** Our relays, preferred first, then our other directories: asked after the contact's own. Any thread. */
  void SetOurRelays(std::vector<std::string> origins);
  /**
   * Whether the contact's card entry `{origin, relay_id}` does not match the relay (our pins disagree, or a send found
   * its audience refused, MR007/MR008): the page says *identity doesn't match*. None: never. Any thread.
   */
  using IdentityMismatchFn =
      std::function<bool(const std::string& account_id, const std::string& origin, const RelayId& relay_id)>;
  void SetIdentityMismatch(IdentityMismatchFn mismatch);

  /**
   * Refreshes `contact_id`; `done` once, on the UI lane. False (and no `done`) when one already runs for it or
   * the contact is unknown.
   */
  bool Start(const std::string& contact_id, std::function<void(ContactRefreshResult)> done);
  bool Running(const std::string& contact_id) const;

  /** The contact's preferred relay and what it answered at the last refresh (the contact page). Any thread. */
  ContactRelayView PreferredRelay(const Contact& contact);

private:
  struct Run;
  void AskRelays(const std::shared_ptr<Run>& run);
  /** The contact's relays in list order, then ours not asked yet, up to the first that knows the account. */
  void AskRelaysInOrder(Run& run);
  /** Asks one relay; true when it answered for the account, verifiably (its answer taken). */
  bool AskRelay(Run& run, const std::string& origin, ContactRefreshResult::RelayAsk::Whose whose);
  /** Takes a relay's row when it verifies for the run's account; false (nothing taken): rejected. */
  bool TakeRelayAnswer(Run& run, StoredAccountRow row);
  enum class CardTaken { Kept, Stale, Rejected };
  CardTaken TakeRelayCard(Run& run, const Object& card);
  void Remember(const std::string& account_id, const std::string& origin,
                ContactRefreshResult::RelayAsk::Outcome outcome);
  void Finish(const std::shared_ptr<Run>& run);

  ContactsStore& contacts_;
  AccountKeyResolver& keys_;
  ContactCardExchange& cards_;
  Post post_blocking_;
  Post post_ui_;
  mutable std::mutex mutex_;
  RelayLookup lookup_;
  IdentityMismatchFn identity_mismatch_;
  std::vector<std::string> our_relays_;
  std::set<std::string> running_;
  /** What each relay last answered, by account and relay (RelayOriginKey); insertion order for forgetting. */
  std::map<std::string, ContactRefreshResult::RelayAsk::Outcome> reach_;
  std::deque<std::string> reach_order_;
  /** Outlives this in the posted steps' captures. */
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
