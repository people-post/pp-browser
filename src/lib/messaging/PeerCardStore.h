#pragma once

#include "lib/messaging/AccountCard.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
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
  /** The union of devices from every account card seen, the newest card's first. */
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
 * Folds a checked account card in: keys and nickname from the newest card, devices as the union (the
 * newest card's first, at most kMaxAccountCardDevices) so a device that does not know a sibling cannot hide it.
 */
PeerCardRecord MergeAccountCard(std::optional<PeerCardRecord> known, const AccountCard& card, const std::string& source);

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

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<std::optional<PeerCardRecord>> GetLocked(sqlite3* db, const std::string& account_id) const;
  Roe<void> PutLocked(sqlite3* db, const PeerCardRecord& record) const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
