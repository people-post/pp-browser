#pragma once

#include "lib/people/AccountBackupCodec.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/RecoverySeed.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What the account locker holds (multi-device M023 §4): account-level state only — never device keys, sessions,
 * private material or messages.
 */
struct AccountLockerState {
  std::string relay_user_id;
  std::string nickname;
  std::vector<Contact> contacts;
  /** Groups (with the latest owner-signed roster statement) and card floors. */
  AccountBackupExtras extras;
  /** Where else this locker is kept (provider base URLs); the configured relay is always tried first. */
  std::vector<std::string> providers;
};

/**
 * One snapshot as providers store it (M023 §4): sealed state, its version, and a signature by the locker signing key
 * over both — under the locker id, the BLAKE2b-256 of that key's public half.
 */
struct LockerSnapshot {
  ByteVector locker_id;
  uint64_t version = 0;
  ByteVector signing_public_key;
  /** XChaCha20-Poly1305 under the locker key (AAD binds the id and the version) of the padded state. */
  ByteVector sealed;
  ByteVector signature;

  bool operator==(const LockerSnapshot&) const = default;
};

/** The locker of one recovery seed: its id and keys. */
struct LockerKeys {
  ByteVector locker_id;
  ByteVector locker_key;
  ByteVector signing_public_key;
  ByteVector signing_secret_key;
};

namespace account_locker {

/** The locker keys a recovery seed's derived keys give (M023 §2). */
Roe<LockerKeys> KeysFrom(const RecoveryKeys& recovery);

/**
 * Seal `state` as snapshot `version`: the state JSON, length-prefixed and zero-padded to a size bucket (4 KiB, then
 * ×4: 16 KiB, 64 KiB, …) so its size says little, AEAD under the locker key, signed.
 */
Roe<LockerSnapshot> Seal(const AccountLockerState& state, const LockerKeys& keys, uint64_t version);

/**
 * What a provider checks before it stores a snapshot, knowing nothing of the account: the signing key hashes to the
 * locker id, and the signature verifies.
 */
Roe<void> VerifyOwner(const LockerSnapshot& snapshot);

/** Verify (VerifyOwner, and that it is this locker's) and open a snapshot. */
Roe<AccountLockerState> Open(const LockerSnapshot& snapshot, const LockerKeys& keys);

/**
 * Of the snapshots providers returned, the newest that verifies and opens — none older than `at_least` (the newest
 * version this device has seen: a provider serving an old copy cannot roll it back). Nullopt when none qualifies.
 */
std::optional<LockerSnapshot> PickNewest(const std::vector<LockerSnapshot>& snapshots, const LockerKeys& keys,
                                         uint64_t at_least = 0);

/** The bytes signed for a snapshot: a tag, the locker id, the version and the sealed state. */
ByteVector SignBytes(const ByteVector& locker_id, uint64_t version, const ByteVector& sealed);

/** The JSON a snapshot's state is (exposed for tests). */
std::string StateToJson(const AccountLockerState& state);
Roe<AccountLockerState> StateFromJson(const std::string& json);

} // namespace account_locker

} // namespace pbr
