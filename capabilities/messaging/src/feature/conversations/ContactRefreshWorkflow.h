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
      Answered,    // it keeps the account: its answer was used
      NotFound,    // it answered, and does not know the account (not registered there)
      Unreachable, // it could not be asked (offline, a 5xx, a bad answer)
    };
    std::string origin;
    Whose whose = Whose::Theirs;
    Outcome outcome = Outcome::Unreachable;

    bool operator==(const RelayAsk&) const = default;
  };
  Devices devices = Devices::NotAsked;
  /** The relays asked, in order: the contact's in list order, then ours, up to the first that knew the account. */
  std::vector<RelayAsk> relays;
  bool name_changed = false;
  bool photo_changed = false;
  bool keys_changed = false;
  bool devices_changed = false;
  /** Something else stored changed (a route, an address). */
  bool other_changed = false;
  /**
   * The relay's answer carried keys (or a card) that do not verify against the contact's Account ID, or answered
   * for another account: its keys were not used (and an answer for another account not at all).
   */
  bool relay_keys_rejected = false;
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
    NotKnown, // no card kept yet, and no locator gave a relay
    MeshOnly, // its card lists no relay: it receives over the mesh only
    Listed,   // `origin` is its preferred relay
  };
  enum class Reach {
    NotKnown,      // not asked since the app started
    Reachable,     // at the last refresh it answered, keeping the account
    NotRegistered, // at the last refresh it answered without the account (our copy of its list may be stale)
    Unreachable,   // at the last refresh it could not be asked
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
  /** The answering relay's row, once it is known to be for this contact's account. */
  std::optional<mesh::DirectoryHit> hit;
  /** The answer's keys verified against its Account ID (and were kept): that Account ID may be adopted. */
  bool hit_keys_verified = false;
};

/**
 * Folds a refresh into `contact` and says what changed. Keys and devices are the account's verified card's
 * (the key stores hold the keys; the card's devices become device rows); the nickname is the kept card's when a
 * card came in now (from a device or a relay), else the relay row's; the photo and the device addresses are the
 * relay row's. Ids: the Account ID is adopted only from an answer whose keys verified. The user's own fields (remark,
 * trust) are never touched.
 */
Contact MergeContactRefresh(Contact contact, const ContactRefreshInputs& inputs, int64_t now_ms,
                            ContactRefreshResult& result);

/**
 * The contact page's Refresh (multi-relay R0, R3): asks, in order of trust, the contact's devices over the mesh for
 * its account's card (a forced ask, ContactCardExchange::AskDevices); then the contact's relays in list order
 * (ContactRelays: its kept card's, else its QR locator's origin), by Account ID (`GET /users/by-account`, public);
 * then our relays (SetOurRelays) not asked already — up to the first relay that keeps the account. A relay's stored
 * card and its row's keys are kept only when they verify against the Account ID (AccountKeyResolver: VerifyAccountCard,
 * VerifyKeyCard); its name and photo count either way. What each relay answered is remembered for the contact page
 * (PreferredRelay). The relay steps run on the blocking lane; the merge, the write and `done` run on the UI lane. One
 * run per contact at a time.
 */
class ContactRefreshWorkflow {
public:
  /** One relay's row for an account, unauthenticated; none: not registered there (LookupStoredAccount). */
  using RelayLookup =
      std::function<Roe<std::optional<StoredAccountRow>>(const std::string& origin, const std::string& account_id)>;
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
  /** Asks one relay; true when it answered for the account (its answer taken). */
  bool AskRelay(Run& run, const std::string& origin, ContactRefreshResult::RelayAsk::Whose whose);
  void TakeRelayAnswer(Run& run, StoredAccountRow row);
  void TakeRelayCard(Run& run, const Object& card);
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
  std::vector<std::string> our_relays_;
  std::set<std::string> running_;
  /** What each relay last answered, by account and relay (RelayOriginKey); insertion order for forgetting. */
  std::map<std::string, ContactRefreshResult::RelayAsk::Outcome> reach_;
  std::deque<std::string> reach_order_;
  /** Outlives this in the posted steps' captures. */
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace pbr
