#pragma once

#include "lib/messaging/PeerCardStore.h"
#include "lib/net/LockerClient.h"
#include "lib/people/AccountLocker.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The account locker as a product step (multi-device M023 s3): publish this account's state to the locker
 * provider (the relay) sealed under keys from the recovery seed, and fetch / open a locker from a seed (restore).
 * The relay half of a write is signed by the account; the snapshot by the locker key. Any thread (blocking I/O).
 */
class AccountLockerWorkflow : public Module {
public:
  /** Where the groups and card floors come from (null-safe: missing ones are left out). */
  struct Sources {
    std::function<Roe<std::vector<AccountStateGroup>>()> groups;
    std::function<Roe<std::vector<PeerCardFloor>>()> card_floors;
  };

  AccountLockerWorkflow(IdentityStore& identity, ContactsStore& contacts, Sources sources);

  /** The provider to write to and read from (the relay); null: none configured. */
  void SetClient(ILockerClient* client) { client_ = client; }
  /** After a sync stored a new version (the account's other devices are worth telling). Any thread. */
  void SetOnWritten(std::function<void()> callback) { on_written_ = std::move(callback); }
  /** Where the highest version this device has seen of each locker is kept (`account_locker.json`; empty: memory). */
  void SetDataDir(std::string data_dir);

  /**
   * Sync the account's state with its locker, both ways (M023 §7): note what changed here since the last sync (the
   * contact clock), take what changed on another device (newer registers win — contacts put in, edited or taken out
   * here), then store the merged state as a version newer than any seen — unless it is what the locker holds already
   * (no write). Errors when the account has no recovery seed (minted before M023) or no relay id, and when the locker
   * cannot be read (writing blind would drop another device's state). The version stored or found.
   */
  Roe<uint64_t> Publish();

  struct Fetched {
    AccountLockerState state;
    uint64_t version = 0;
  };
  /**
   * The state a recovery seed's locker holds, verified and opened: the newest valid copy among the provider's and
   * `saved_copies` (exported locker files' text) — never older than the highest version this device has seen of
   * that locker, which it raises; nullopt when there is none. With saved copies, an unreachable provider is not an
   * error; a file that is not a saved account is.
   */
  Roe<std::optional<Fetched>> Fetch(const ByteVector& recovery_seed, const std::vector<std::string>& saved_copies = {});

  /**
   * The account's state as this device has it, sealed as a locker snapshot (a version above any seen) inside an
   * exported locker file's JSON — the user keeps it where they like (M023 §6 amendment). No network.
   */
  Roe<std::string> ExportFileJson();

  /** The highest version this device has seen of a locker (hex id; published or fetched); 0 when none. */
  uint64_t HighestSeen(const std::string& locker_id_hex) const;

  /** The account state as it is here now (what Publish seals). */
  Roe<AccountLockerState> CurrentState();

private:
  IdentityStore& identity_;
  ContactsStore& contacts_;
  Sources sources_;
  void RaiseHighestSeen(const std::string& locker_id_hex, uint64_t version);
  /** `account_locker.json`: the highest versions seen and the contact clock. */
  void Save();
  /** The remote registers that win, applied to the address book. */
  Roe<void> TakeRemoteContacts(const std::vector<Contact>& local, const AccountLockerState& stored);

  ILockerClient* client_ = nullptr;
  std::function<void()> on_written_;
  std::string data_dir_;
  mutable std::mutex mutex_;
  /** Serializes Publish (read, merge, write). */
  std::mutex sync_mutex_;
  ContactClock clock_;
  /** Per locker (hex id): a provider serving an older copy of one locker says nothing about another. */
  std::map<std::string, uint64_t> highest_seen_;
};

/** Provider text ↔ snapshot bytes (hex id, base64 fields). */
LockerRecord LockerRecordFromSnapshot(const LockerSnapshot& snapshot);
Roe<LockerSnapshot> LockerSnapshotFromRecord(const LockerRecord& record);
/** The record inside an exported locker file; an error when the text is not one. */
Roe<LockerRecord> LockerRecordFromFileJson(const std::string& file_json);

} // namespace pbr
