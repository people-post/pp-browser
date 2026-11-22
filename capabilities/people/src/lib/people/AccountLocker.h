#pragma once

#include "lib/people/AccountStateCodec.h"
#include "lib/people/ContactClock.h"
#include "lib/people/ContactTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/RecoverySeed.h"
#include "foundation/crypto/SignedReceive.h"

#include "common/Error.h"
#include "common/ExtArea.h"
#include "common/Value.h"

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

/**
 * Most grants a locker keeps (multi-relay MR005, ACCOUNT_LOCKER § State JSON `grants`): about 4.6 KB each (the
 * signature), so 32 keep the snapshot within a small size bucket. Over it, the latest issued stay: a grantor that
 * still writes to us renews, and one cut here grants a restored device again once it hears of it.
 */
inline constexpr size_t kMaxLockerGrants = 32;
/** The relays a locker's `relays` carries at most (a fifth and on are dropped on read; MR004 bounds a list to 4). */
inline constexpr size_t kMaxLockerRelays = 4;
/** Longest grant field read from a locker (an Account ID, a typed ML-DSA-65 signature's base64 fits): longer drops it. */
inline constexpr size_t kMaxLockerGrantFieldBytes = 8192;

/**
 * The locker record's version (release-compatibility SIGNED_FORMATS § 2.6): outside, the record's `record_version`;
 * inside, the byte after the signing domain. Release 1 reads this one only.
 */
inline constexpr uint8_t kLockerRecordVersion = 2;
/** The record's signing domain (no `-vN`: the version is the byte after it). */
inline constexpr char kLockerRecordDomain[] = "pp-browser:account-locker";
/**
 * The state JSON's version (`state_version`, inside the seal; INVENTORY T7): read first once a snapshot opens. A
 * higher one is a newer app's: this device says "update needed" and does not write the locker.
 */
inline constexpr int64_t kLockerStateVersion = 1;

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
  /** Display names of the account's devices (key `device_names`), sorted by peer_id. */
  std::vector<DeviceNameEntry> device_names;
  /**
   * The state JSON this state was read from (StateFromJson), empty for one built here. A writer keeps what it does not
   * know (T7): StateToJson writes back its top-level keys this build does not read, and each row's unknown fields on
   * the row of the same key. MergeForPublish carries the stored copy's.
   */
  Object kept;
};

/**
 * One snapshot as providers store it: locker record v2 (SIGNED_FORMATS § 2.6; docs/contracts/ACCOUNT_LOCKER.md
 * § Record): sealed state, its revision, and a signature by the locker signing key over both and the extension area,
 * under the locker id, the BLAKE2b-256 of that key's raw public half.
 */
struct LockerSnapshot {
  /** The outside version (`record_version`): kLockerRecordVersion for every snapshot this build reads or writes. */
  int64_t record_version = kLockerRecordVersion;
  ByteVector locker_id;
  /** The revision (JSON `version`): the newest wins. */
  uint64_t version = 0;
  /** The locker signing key, typed (algorithm id, then ML-DSA-65). */
  ByteVector signing_public_key;
  /**
   * XChaCha20-Poly1305 under the locker key (AAD binds the id and the revision) of the padded state: `FileCipher`'s
   * bytes, whose first byte is the AEAD id.
   */
  ByteVector sealed;
  /** The signed extension area (`ext_b64`): none is defined at release 1; a received record keeps its entries. */
  pp::ExtArea ext;
  /** Typed signature by the locker signing key. */
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
 * What a provider checks before it stores a snapshot, knowing nothing of the account, in the receiver's order
 * (docs/contracts/SIGNED_EXTENSIONS.md): `record_version`, the algorithm ids of the key, the signature and the sealed
 * bytes, the signature (the raw key hashes to the locker id, and it verifies), an unknown critical entry. "Update
 * needed" (a newer version, an unknown algorithm or critical entry) is `AppError::NewerFormat`.
 */
Roe<void> VerifyOwner(const LockerSnapshot& snapshot);

/**
 * Verify (VerifyOwner, and that it is this locker's) and open a snapshot; then `state_version` first. A newer record
 * or state is `AppError::NewerFormat`.
 */
Roe<AccountLockerState> Open(const LockerSnapshot& snapshot, const LockerKeys& keys);

/** What PickNewest found. */
struct PickedSnapshot {
  /** The newest snapshot that verifies and opens, none older than `at_least`. */
  std::optional<LockerSnapshot> newest;
  /** A snapshot a newer app wrote was among them (skipped): this device stops publishing ("update needed"). */
  bool newer_format = false;
};

/**
 * Of the snapshots providers returned, the newest that verifies and opens — none older than `at_least` (the newest
 * version this device has seen: a provider serving an old copy cannot roll it back). One a newer app wrote (a newer
 * record or state version, an unknown critical entry) is skipped and noted.
 */
PickedSnapshot PickNewest(const std::vector<LockerSnapshot>& snapshots, const LockerKeys& keys, uint64_t at_least = 0);

/**
 * What to publish when the locker already holds `stored` (another device of the account wrote it): contacts by their
 * clocks (the newer register per account wins, ours on a tie; a removal is a tombstone and carries; a contact neither
 * side has a register for is kept from either), groups by id (the higher roster epoch wins, ours on a tie), card
 * floors (the newest per account), grants (both copies' — the publisher runs CapGrants), relays (the copy from the
 * newer card; ours on a tie), providers (union).
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

/**
 * The grants a locker keeps, from any mix of copies: none expired at `now_ms`, one per grantor (the latest issued;
 * the larger signature on a tie, so every device keeps the same), at most kMaxLockerGrants (the latest issued stay),
 * sorted by grantor. Pure. Checking the signatures is the reader's job.
 */
std::vector<AccountStateGrant> CapGrants(std::vector<AccountStateGrant> grants, int64_t now_ms);

/** `device_names` as JSON (an array of {peer_id, name, at}; a tombstone is {peer_id, at, removed: true}). */
Value DeviceNamesToJson(const std::vector<DeviceNameEntry>& entries);
/**
 * Read an untrusted `device_names` array: each live name through NormalizeDeviceName (an empty one drops the
 * entry; a tombstone's name is ignored), `at` clamped to [0, kMaxDeviceNameAt], unknown fields ignored, one entry
 * per peer_id (the larger `at`), capped by CapDeviceNames. Tombstones are kept.
 */
std::vector<DeviceNameEntry> DeviceNamesFromJson(const Array& rows);

/**
 * The bytes signed for a snapshot: `"pp-browser:account-locker" 00 ‖ u8 2 ‖ LenBytes locker_id ‖ u64 version ‖
 * LenBytes sealed ‖ ext area` (u64 BE lengths) — what a provider checks (the relay's `crypto/locker.ts`). The signing
 * key is not in them: the locker id it hashes to is.
 */
ByteVector SignBytes(const ByteVector& locker_id, uint64_t version, const ByteVector& sealed, const pp::ExtArea& ext);

/**
 * The record JSON (`POST /v1/lockers/put`'s and `GET /v1/lockers/:id`'s `locker`, a saved copy's `locker`):
 * `{record_version: 2, locker_id (lowercase hex), version, signing_public_key_b64, sealed_b64, ext_b64 (when not
 * empty), signature_b64}`.
 */
Object RecordToJson(const LockerSnapshot& snapshot);
/**
 * Steps 1–3 of the receiver's order for a record JSON: `record_version` first (missing or older: refused; newer:
 * `AppError::NewerFormat`, nothing else read), the fields and the area (malformed: refused), the typed blobs' ids
 * (unknown: NewerFormat). VerifyOwner checks the signature.
 */
Roe<LockerSnapshot> RecordFromJson(const Object& json);
/**
 * The whole receiver's order for a record JSON, as a provider runs it (the relay's `lockers/put`): steps 1–3 as
 * RecordFromJson, the signature (the raw key hashes to the locker id, and it verifies), an unknown critical entry.
 * `received` is filled when accepted. VerifyOwner is this, of a snapshot's JSON.
 */
SignedReceipt ReceiveRecord(const Object& json, LockerSnapshot* received);

/**
 * The JSON a snapshot's state is (`state_version` first; exposed for tests), with what `state.kept` holds that this
 * build does not read.
 */
std::string StateToJson(const AccountLockerState& state);
/** `state_version` first (missing or older: refused; newer: `AppError::NewerFormat`), then the state. */
Roe<AccountLockerState> StateFromJson(const std::string& json);

} // namespace account_locker

} // namespace pbr
