#pragma once

#include "lib/people/AccountCard.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What this device knows about an account from the cards it has checked (docs/contracts/ACCOUNT_CARD.md). */
struct PeerCardRecord {
  KeyCard keys;
  std::string nickname;
  /** The newest account card's; 0 while only a key card (the directory's, a first message's) is known. */
  int64_t issued_at = 0;
  /** The newest account card's devices (its PeerIds): the account's device list (M021). */
  std::vector<std::string> devices;
  /** Where the newest information came from (directory, envelope, contact_card, device, locator). */
  std::string source;
  /** The newest signed account card as received, to pass on (a contact card); none while only key cards are known. */
  std::optional<AccountCard> newest_card;

  bool operator==(const PeerCardRecord&) const = default;
};

/**
 * Folds a checked key card into what is known: it fills the keys while no account card has been seen
 * (a signed, dated card outranks an undated key card; KEM rotation is M019's).
 */
PeerCardRecord MergeKeyCard(std::optional<PeerCardRecord> known, const KeyCard& card, const std::string& source);
/**
 * Folds a checked account card in: the newest card (`issued_at`) is the account's word on its keys,
 * nickname and device list (M021) — an older one changes nothing.
 */
PeerCardRecord MergeAccountCard(std::optional<PeerCardRecord> known, const AccountCard& card, const std::string& source);

/**
 * The newest account card this device knew for an account, by issue time only: kept in the account locker so
 * a restored device, which keeps no cards, still refuses an older card than it had seen (one listing a device
 * since taken out). See docs/contracts/ACCOUNT_CARD.md#newest-wins.
 */
struct PeerCardFloor {
  std::string account_id;
  int64_t issued_at = 0;

  bool operator==(const PeerCardFloor&) const = default;
};

/** What a message carries for a kept account: its newest account card, else its key card. */
Object CarriedCardJson(const PeerCardRecord& record);

/** A kept record as stored (its JSON form); `PeerCardRecordFromJson` reads it back. */
Object PeerCardRecordToJson(const PeerCardRecord& record);
Roe<PeerCardRecord> PeerCardRecordFromJson(const Object& json);

/**
 * Where a device keeps peers' cards (CR024): one record per account, and the floors restored from the account
 * locker. The rows only — what a card may change is PeerCards'. The app's storage keeps them (`peer_cards`; public
 * material, plaintext like contacts.json); tests use an in-memory one.
 */
class IPeerCardStore {
public:
  virtual ~IPeerCardStore() = default;

  virtual Roe<std::optional<PeerCardRecord>> Get(const std::string& account_id) const = 0;
  virtual Roe<std::vector<PeerCardRecord>> List() const = 0;
  virtual Roe<void> Put(const PeerCardRecord& record) = 0;
  /** An account's floor (0 when none). */
  virtual Roe<int64_t> Floor(const std::string& account_id) const = 0;
  /** Raises each floor to at least its issue time (a lower one changes nothing). */
  virtual Roe<void> RaiseFloors(const std::vector<PeerCardFloor>& floors) = 0;
  virtual Roe<std::vector<PeerCardFloor>> Floors() const = 0;
};

/**
 * Peers' cards as this device knows them (docs/contracts/ACCOUNT_CARD.md): every card is checked before it is kept,
 * the newest account card is the account's word, and one older than a restored floor is refused. Thread-safe: a
 * card's read-then-write is whole.
 */
class PeerCards : public Module {
public:
  /** `store` keeps the rows (and must outlive this). */
  explicit PeerCards(IPeerCardStore& store);

  Roe<PeerCardRecord> AddKeyCard(const KeyCard& card, const std::string& source);
  Roe<PeerCardRecord> AddAccountCard(const AccountCard& card, int64_t now_ms, const std::string& source);
  Roe<std::optional<PeerCardRecord>> Get(const std::string& account_id) const;
  Roe<std::vector<PeerCardRecord>> List() const;

  /**
   * Floors restored from the account locker: from now on an account card older than its floor is refused
   * (AddAccountCard), and a carried one does not speak for its sender (Floor). A lower floor changes nothing.
   */
  Roe<void> RaiseFloors(const std::vector<PeerCardFloor>& floors);
  /** The issue time an account card of `account_id` must reach: its restored floor, else 0. */
  Roe<int64_t> Floor(const std::string& account_id) const;
  /** Per account, the newest issue time known — a kept account card's, or a floor's — for the account locker. */
  Roe<std::vector<PeerCardFloor>> NewestKnown() const;
  /** Called (outside the lock) when an account's newest card got newer: the account locker keeps its floor. */
  void SetOnNewerCard(std::function<void()> callback);

private:
  IPeerCardStore& store_;
  mutable std::mutex mutex_;
  std::function<void()> on_newer_card_;
};

} // namespace pbr
