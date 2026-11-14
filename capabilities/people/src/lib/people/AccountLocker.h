#pragma once

#include "lib/people/AccountStateCodec.h"
#include "lib/people/ContactClock.h"
#include "lib/people/ContactTypes.h"
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
 * Most entries (live names and removal tombstones together) a locker keeps: the card lists at most
 * kMaxAccountCardDevices, the rest is room for tombstones. One limit, applied the same way on every device.
 */
inline constexpr size_t kMaxDeviceNameEntries = 8;

/** The furthest `at` read from a locker or file (Unix ms, the year 2100): anything beyond is cut back to it. */
inline constexpr int64_t kMaxDeviceNameAt = 4102444800000;

/** One device's display name inside the locker (never in the account card, a log or a metric). */
struct DeviceNameEntry {
  std::string peer_id;
  /** Never empty for a live entry, always through NormalizeDeviceName; empty for a tombstone. */
  std::string name;
  /** Unix ms, within [0, kMaxDeviceNameAt]; per peer_id the larger `at` wins, whoever wrote it (a tombstone too). */
  int64_t at = 0;
  /** A removal tombstone: the device was taken off the account. Carries no name; shown nowhere. */
  bool removed = false;

  bool operator==(const DeviceNameEntry&) const = default;
};

/**
 * What the account locker holds (multi-device M023 §4): account-level state only — never device keys, sessions,
 * private material or messages.
 */
struct AccountLockerState {
  std::string nickname;
  std::vector<Contact> contacts;
  /** Groups (with the latest owner-signed roster statement) and card floors. */
  AccountStateExtras extras;
  /** Where else this locker is kept (provider base URLs); the configured relay is always tried first. */
  std::vector<std::string> providers;
  /** Each contact's sync register (M023 §7): the newest wins per account, a removal is a tombstone. */
  std::vector<ContactClockEntry> contact_clock;
  /** Display names of the account's devices (key `device_names`), sorted by peer_id; absent in older snapshots. */
  std::vector<DeviceNameEntry> device_names;
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

/**
 * What to publish when the locker already holds `stored` (another device of the account wrote it): contacts by their
 * clocks (the newer register per account wins, ours on a tie; a removal is a tombstone and carries; a contact neither
 * side has a register for is kept from either), groups by id (the higher roster epoch wins, ours on a tie), card
 * floors (the newest per account), providers (union).
 */
AccountLockerState MergeForPublish(const AccountLockerState& ours, const AccountLockerState& stored);

/**
 * Device names of two copies by peer_id: the larger `at` wins whoever wrote it, `remote` on a tie (a sync with
 * nothing newer changes nothing); a copy without a key never erases the other's. Sorted by peer_id.
 */
std::vector<DeviceNameEntry> MergeDeviceNames(const std::vector<DeviceNameEntry>& local,
                                              const std::vector<DeviceNameEntry>& remote);

/**
 * At most kMaxDeviceNameEntries, sorted by peer_id. Pure — no card, no "own" exception — so every device cuts the
 * same entries: over the limit, tombstones go first (the oldest `at` first), then live entries (the smallest `at`
 * first); the larger peer_id goes first on a tie. Never prunes below the limit.
 */
std::vector<DeviceNameEntry> CapDeviceNames(std::vector<DeviceNameEntry> entries);

/** `device_names` as JSON (an array of {peer_id, name, at}; a tombstone is {peer_id, at, removed: true}). */
Value DeviceNamesToJson(const std::vector<DeviceNameEntry>& entries);
/**
 * Read an untrusted `device_names` array: each live name through NormalizeDeviceName (an empty one drops the
 * entry; a tombstone's name is ignored), `at` clamped to [0, kMaxDeviceNameAt], unknown fields ignored, one entry
 * per peer_id (the larger `at`), capped by CapDeviceNames. Tombstones are kept.
 */
std::vector<DeviceNameEntry> DeviceNamesFromJson(const Array& rows);

/**
 * The bytes signed for a snapshot: `pp-browser:account-locker-v1` and a zero byte, version byte 1, LenBytes(locker
 * id), u64 BE version, LenBytes(sealed) — what a provider checks (the relay: web2 `www` relay/AccountLocker.ts).
 */
ByteVector SignBytes(const ByteVector& locker_id, uint64_t version, const ByteVector& sealed);

/** The JSON a snapshot's state is (exposed for tests). */
std::string StateToJson(const AccountLockerState& state);
Roe<AccountLockerState> StateFromJson(const std::string& json);

} // namespace account_locker

} // namespace pbr
