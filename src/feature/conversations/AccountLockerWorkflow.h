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
#include <optional>
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
    std::function<Roe<std::vector<AccountBackupGroup>>()> groups;
    std::function<Roe<std::vector<PeerCardFloor>>()> card_floors;
  };

  AccountLockerWorkflow(IdentityStore& identity, ContactsStore& contacts, Sources sources);

  /** The provider to write to and read from (the relay); null: none configured. */
  void SetClient(ILockerClient* client) { client_ = client; }

  /**
   * Seal the account's current state and store it as a version newer than any this device published (its clock in
   * ms, or one past the last). Errors when the account has no recovery seed (minted before M023) or no relay id.
   */
  Roe<uint64_t> Publish();

  /** The state a recovery seed's locker holds at the provider, verified and opened; nullopt when there is none. */
  Roe<std::optional<AccountLockerState>> Fetch(const ByteVector& recovery_seed, uint64_t at_least = 0);

  /** The account state as it is here now (what Publish seals). */
  Roe<AccountLockerState> CurrentState();

private:
  IdentityStore& identity_;
  ContactsStore& contacts_;
  Sources sources_;
  ILockerClient* client_ = nullptr;
  uint64_t last_published_ = 0;
};

/** Provider text ↔ snapshot bytes (hex id, base64 fields). */
LockerRecord LockerRecordFromSnapshot(const LockerSnapshot& snapshot);
Roe<LockerSnapshot> LockerSnapshotFromRecord(const LockerRecord& record);

} // namespace pbr
