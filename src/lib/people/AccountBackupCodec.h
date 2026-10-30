#pragma once

#include "lib/people/ContactTypes.h"
#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** One group member as the backup keeps it. */
struct AccountBackupGroupMember {
  std::string identity;
  /** `owner` / `admin` / `member`. */
  std::string role;
  int64_t joined_at = 0;
  std::string contact_id;
};

/**
 * A group as the backup keeps it (group-chat G013): its metadata, its roster as identities and the
 * latest owner-signed roster statement — no messages, no numbering (it restarts per pair, G010). A
 * restored group checks its members before it sends.
 */
struct AccountBackupGroup {
  std::string group_id;
  std::string title;
  std::string owner_identity;
  uint64_t roster_epoch = 0;
  uint64_t title_version = 0;
  std::string policy_json;
  std::vector<AccountBackupGroupMember> members;
  /** The owner-signed roster statement JSON held; empty when none. */
  std::string roster_statement_json;
};

/**
 * How new the newest account card of one account was on the device that wrote the backup (issue time only,
 * not the card). A restored device keeps no cards; with this it still refuses an older card than it had seen
 * — one listing a device taken out since (ACCOUNT_CARD § Newest wins).
 */
struct AccountBackupCardFloor {
  std::string account_id;
  int64_t issued_at = 0;

  bool operator==(const AccountBackupCardFloor&) const = default;
};

/** What the backup keeps beyond the account and its contacts; each part optional on the wire. */
struct AccountBackupExtras {
  std::vector<AccountBackupGroup> groups;
  std::vector<AccountBackupCardFloor> card_floors;
};

/** The secret part of the backup: sealed under the profile DEK, never stored in the clear. */
struct AccountBackupContents {
  /** Serialized `LinkDeviceBundleV1` (account keys, DEK, relay id, nickname, public PSKs). */
  std::string link_bundle_json;
  std::vector<Contact> contacts;
  /** Optional `groups` and `card_floors` (absent in backups before them; an older app ignores them). */
  AccountBackupExtras extras;
};

/**
 * Local account backup (DESIGN-account-recovery §9–10), kept where an uninstall does not delete it
 * (iOS Keychain). Only the nickname is in the clear; the rest opens with the profile PIN: `vault` is
 * the `vault.bin` bytes (DEK wrapped under the PIN), `sealed` the contents under that DEK.
 */
struct AccountBackupV2 {
  int64_t updated_at_ms = 0;
  std::string nickname;
  /** Profile id the vault and the sealed contents are bound to (AAD). */
  std::string profile_id;
  std::vector<uint8_t> vault;
  std::vector<uint8_t> sealed;
};

class AccountBackupCodec {
public:
  static constexpr int kSchemaVersion = 2;
  /** Name of the blob in the secure store. */
  static constexpr const char kBlobName[] = "account-backup";

  static Roe<std::vector<uint8_t>> Serialize(const AccountBackupV2& backup);
  static Roe<AccountBackupV2> Parse(const std::vector<uint8_t>& bytes);

  /** Plaintext of `AccountBackupV2::sealed`. */
  static Roe<std::vector<uint8_t>> SerializeContents(const AccountBackupContents& contents);
  static Roe<AccountBackupContents> ParseContents(const std::vector<uint8_t>& bytes);
  /** One group as the contents carry it. */
  static Object GroupToJson(const AccountBackupGroup& group);
  static std::optional<AccountBackupGroup> GroupFromJson(const Object& json);
};

} // namespace pbr
