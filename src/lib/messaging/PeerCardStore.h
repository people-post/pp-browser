#pragma once

#include "lib/messaging/AccountCard.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

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

/**
 * Persisted cards, one row per account in profile.db (`peer_cards`; public material, plaintext like
 * contacts.json). Every card is checked before it is kept. Thread-safe.
 */
class PeerCardStore : public Module {
public:
  explicit PeerCardStore(std::string profile_db_path);

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
  /** Called (outside the store's lock) when an account's newest card got newer: the account locker keeps its floor. */
  void SetOnNewerCard(std::function<void()> callback);

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<std::optional<PeerCardRecord>> GetLocked(sqlite3* db, const std::string& account_id) const;
  Roe<void> PutLocked(sqlite3* db, const PeerCardRecord& record) const;
  Roe<int64_t> FloorLocked(sqlite3* db, const std::string& account_id) const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
  std::function<void()> on_newer_card_;
};

} // namespace pbr
