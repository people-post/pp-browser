#pragma once

#include "common/directory/KeyCard.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/people/AccountCard.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a device that its account's newest list leaves out is told (M021 d4). */
inline constexpr const char kSignedOutMessage[] =
    "This device was signed out of your account. Link it again from your main device to use it.";
/** A linked device whose kept card does not verify (stepped up on disk): the main device's newer card comes first. */
inline constexpr const char kAwaitingMainCardMessage[] = "account card: waiting for the main device's card";
/** A device that holds no card of its account (and starts none): it is getting it back from our relays. */
inline constexpr const char kAwaitingOwnCardMessage[] = "Getting your device list…";
/** No relay that answered keeps a card of ours: the person decides whether to start a new list (StartFreshList). */
inline constexpr const char kNoOwnCardMessage[] = "Your device list was not found";

/** Where a device that holds no card of its own stands in getting it back (OwnDeviceList::recovery). */
enum class OwnCardRecovery {
  None,     // a card is kept, or one is being started (`card_intent`)
  Getting,  // asking our relays (or waiting to ask again: none answered)
  NotFound, // every relay that answered keeps none: the person is asked
};

/** What our relays' directories answered for our own account (OwnCards::OwnCardLookup). */
struct OwnCardAnswers {
  /** The account cards served for our account, as served: unchecked. */
  std::vector<Object> cards;
  /** The relays that answered — with a card, or that they keep none. 0: none reached (offline). */
  size_t answered = 0;
};

/** The account's devices as this device knows them (M021 d4): the newest card's list; its first is the master. */
struct OwnDeviceList {
  std::vector<AccountCardDevice> devices;
  std::string self_peer_id;
  /** The newest list leaves this device out (and it is not joining or starting a fresh list). */
  bool signed_out = false;
  /** No card of ours is kept: where getting it back stands. */
  OwnCardRecovery recovery = OwnCardRecovery::None;
};

/**
 * This account's own cards (docs/contracts/ACCOUNT_CARD.md): the key card, and the account card — the
 * account's device list (M021), kept with the other accounts' cards. The newest card wins (no union). Its first
 * device is the **master** (d4): only it changes the list — takes a device out, hands the role on. Any listed
 * device re-issues the card to bring its own entry, the nickname or the initiation floor up to date, the list
 * otherwise as it is; a linked device appends itself once (`card_intent` join), a restored one starts a fresh list
 * holding only itself (`fresh`) — as a new account does. The relay list (MR002) is the master's: the relays it registers
 * on (SetRelays); the other devices keep the newest card's. A device the newest list leaves out is signed out: it
 * issues nothing.
 *
 * Our newest card is kept twice: beside the identity (IdentityStore::OwnCard, which a reset of the chat store leaves)
 * and in the chat store's `peer_cards` with everyone else's; each read brings the older copy up to the newer. A device
 * that holds neither and has no `card_intent` starts no list: it gets its card back from our relays' directories
 * (SetOwnCardLookup), checked against our account key, and asks the person only when every relay that answered keeps
 * none (StartFreshList). Thread-safe.
 */
class OwnCards : public Module {
public:
  OwnCards(IdentityStore& identity, AccountKeyResolver& cards);
  ~OwnCards() override;
  OwnCards(const OwnCards&) = delete;
  OwnCards& operator=(const OwnCards&) = delete;

  Roe<KeyCard> Keys();
  /**
   * The account card: the newest this device knows, re-issued when this device's entry or the nickname changed
   * (or to join / start fresh, per `card_intent`). An error (kSignedOutMessage) when this device is signed out, and
   * (kAwaitingMainCardMessage) on a linked device whose kept card does not verify: only the main device re-issues it
   * (its relays are the main device's to give). With no card kept and no intent to start one, nothing is issued: it
   * asks our relays for our card (at most once per backoff) and says kAwaitingOwnCardMessage, or kNoOwnCardMessage
   * once every relay that answered keeps none.
   */
  Roe<AccountCard> Card(int64_t now_ms);
  /**
   * The person chose to start a new device list (Me → My devices, while no card of ours is found): this device's next
   * card holds only itself, as the master — the account's other devices are signed out. Refused while a card of ours
   * is kept.
   */
  Roe<AccountCard> StartFreshList(int64_t now_ms);
  /**
   * The network came back, or the app came to the front: a device still without its card asks our relays again now,
   * not after its backoff (also after NotFound: a relay that was down may have it). Nothing otherwise.
   */
  void RetryRecovery(int64_t now_ms);
  /**
   * Brings the two copies of our newest card together (the identity's, the chat store's) without issuing one: after a
   * reset of the chat store, our card is back in `peer_cards` for everything else that reads cards.
   */
  void Reconcile(int64_t now_ms);
  /**
   * A card of our own account from a sibling (the link bundle, a sibling's notice): checked and kept; the newest
   * wins. The nickname is the account's: the newest card's becomes this device's too. Not an error when it leaves
   * this device out (see SignedOut).
   */
  Roe<AccountCard> Learn(const AccountCard& card, int64_t now_ms);
  /** The account's devices as this device knows them. */
  Roe<OwnDeviceList> Devices();
  /** The newest list leaves this device out: it stops sending (M021 d4). */
  bool SignedOut();
  /** The master takes another device off the list (revoke, M021 d4); an error on any other device. */
  Roe<AccountCard> RemoveDevice(const std::string& peer_id, int64_t now_ms);
  /** The master hands its role to another listed device (moves it first). */
  Roe<AccountCard> MakeMaster(const std::string& peer_id, int64_t now_ms);
  /**
   * The master gave out a transfer code: until `until_ms` a device that an older card of ours lists and the newest
   * does not is taken as joining (appended), not as signed out — the new device's card may reach the master after
   * the master issued another (a rename). In memory: a restart, or a restored master, expects none.
   */
  void ExpectJoin(int64_t until_ms);
  /** This device's entry on the card. */
  Roe<AccountCardDevice> Self();
  /** This device's PeerId (its mesh identity, from the device key). */
  Roe<std::string> DevicePeerId();
  /**
   * The relays this device registers on, preferred first, each with the PeerId pinned for it (multi-relay MR007; the
   * caller leaves out a relay not pinned yet or whose identity changed): the master's card lists the ones within
   * MR004's and MR007's bounds (an `http://` lab relay is left out), and its next Card() re-issues when they changed.
   * True when the card's relays changed.
   */
  bool SetRelays(const std::vector<AccountCardRelay>& relays);
  /**
   * Runs when the account's card changed here — this device issued one, or took a sibling's newer one — so its
   * siblings hear of it and its own devices are reached; outside any lock.
   */
  void SetOnChanged(std::function<void(const AccountCard&)> on_changed) { on_changed_ = std::move(on_changed); }
  /**
   * Runs on the master when a card it takes in lists a device it did not (one just linked): the account's contacts
   * are worth telling (M021 d3) — the master has them; the new device may not yet. Outside any lock.
   */
  void SetOnDeviceJoined(std::function<void(const AccountCard&)> on_joined) { on_joined_ = std::move(on_joined); }
  /**
   * How our card is asked of our relays' directories (the feature's wiring: our relays, preferred first, then the
   * org's; `done` once, any thread, never under this object's lock). None set: a device without its card waits.
   */
  using OwnCardLookup = std::function<void(const std::string& account_id, std::function<void(OwnCardAnswers)> done)>;
  void SetOwnCardLookup(OwnCardLookup lookup);
  /** Runs when where getting our card back stands changed (OwnDeviceList::recovery); outside any lock. */
  void SetOnRecoveryChanged(std::function<void()> changed);

  /** The first wait before asking our relays again when none answered; doubles to kRecoveryMaxRetryMs. */
  static constexpr int64_t kRecoveryFirstRetryMs = 5 * 1000;
  static constexpr int64_t kRecoveryMaxRetryMs = 5 * 60 * 1000;

private:
  /** What a card says besides its keys: what IssueLocked compares and signs. */
  struct CardContent {
    std::vector<AccountCardDevice> devices;
    std::string nickname;
    std::vector<AccountCardRelay> relays;
    int64_t initiation_floor = 0;
  };
  /** The card saying `content`: `base` when it says it all (and verifies), else a new one (`issued` set). */
  Roe<AccountCard> IssueLocked(const std::optional<AccountCard>& base, CardContent content, int64_t now_ms,
                               bool& issued);
  /** `base` was signed by the account for what it says; checked once per signature. */
  bool VerifiedLocked(const AccountCard& base, int64_t now_ms);
  /** The relays a card issued by this device lists: its own when it is the master, else the newest card's. */
  std::vector<AccountCardRelay> RelaysLocked(const std::vector<AccountCardDevice>& devices, const std::string& self,
                                             const std::optional<AccountCard>& newest) const;
  Roe<AccountCard> ChangeList(const std::string& peer_id, bool make_master, int64_t now_ms);
  /** Our newest card: the newer of the identity's copy and the chat store's, the older brought up to it. */
  Roe<std::optional<AccountCard>> NewestLocked(int64_t now_ms);
  /** No card kept, no intent: whether to ask our relays now (none in flight, the backoff over; not after NotFound). */
  bool RecoveryDueLocked(int64_t now_ms) const;
  /** Asks our relays for our card (outside the lock). */
  void StartRecovery(const std::string& account_id);
  /** Their answers: the newest that checks out against our account key is kept, else NotFound or a backoff. */
  void OnRecoveryAnswers(const OwnCardAnswers& answers, int64_t now_ms);
  /** The newest of `answers`' cards that is ours: our Account ID and account keys, and its signature verifies. */
  std::optional<AccountCard> OursAmong(const OwnCardAnswers& answers, int64_t now_ms);
  void NotifyRecoveryChanged();

  IdentityStore& identity_;
  AccountKeyResolver& cards_;
  std::mutex mutex_; // issuing a card
  std::mutex keys_mutex_;
  std::optional<KeyCard> keys_;
  std::function<void(const AccountCard&)> on_changed_;
  std::function<void(const AccountCard&)> on_joined_;
  int64_t join_expected_until_ms_ = 0;   // under mutex_
  std::vector<AccountCardRelay> relays_; // under mutex_
  std::string verified_signature_;       // under mutex_: the kept card last found signed
  OwnCardLookup lookup_;                 // under mutex_
  std::function<void()> on_recovery_changed_;
  OwnCardRecovery recovery_ = OwnCardRecovery::None; // under mutex_: only meaningful while no card is kept
  bool recovery_in_flight_ = false;                  // under mutex_
  int64_t recovery_retry_at_ms_ = 0;                 // under mutex_
  int64_t recovery_backoff_ms_ = 0;                  // under mutex_
  /** What a lookup's answer reaches: this object while it lives (an answer may come after a teardown). */
  struct Life {
    std::mutex mutex;
    OwnCards* owner = nullptr;
  };
  std::shared_ptr<Life> life_;
};

} // namespace pbr
