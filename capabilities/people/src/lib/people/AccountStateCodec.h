#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** One group member as the account locker keeps it (multi-device M023 §4). */
struct AccountStateGroupMember {
  std::string identity;
  /** `owner` / `admin` / `member`. */
  std::string role;
  int64_t joined_at = 0;
  std::string contact_id;
};

/**
 * A group as the account locker keeps it (M023 §4, group-chat G013): its metadata, its roster as identities and
 * the latest owner-signed roster statement — no messages, no numbering (it restarts per pair, G010). A group
 * restored from the locker checks its members before it sends.
 */
struct AccountStateGroup {
  std::string group_id;
  std::string title;
  std::string owner_identity;
  uint64_t roster_epoch = 0;
  uint64_t title_version = 0;
  std::string policy_json;
  std::vector<AccountStateGroupMember> members;
  /** The owner-signed roster statement JSON held; empty when none. */
  std::string roster_statement_json;
  /**
   * The account is out of this group (left, or removed) as of `roster_epoch`: a tombstone (id, owner and epoch
   * only) so an older copy where it was a member does not bring the group back. A restore skips it.
   */
  bool left = false;
  /**
   * The owner ended the group (G016): a tombstone like `left`, and terminal — a copy with a higher roster epoch or one
   * where the account was a member does not bring it back (it carries no roster to restore).
   */
  bool dissolved = false;
};

/**
 * How new the newest account card of one account was on the device that wrote the locker (issue time only, not
 * the card). A restored device keeps no cards; with this it still refuses an older card than the account had
 * seen — one listing a device taken out since (ACCOUNT_CARD § Newest wins).
 */
struct AccountCardFloor {
  std::string account_id;
  int64_t issued_at = 0;

  bool operator==(const AccountCardFloor&) const = default;
};

/**
 * A grant another account gave this one, as received (multi-relay MR005: it lets this account write to the grantor's
 * relay inboxes past the stranger rules): relay grant v2 (release-compatibility SIGNED_FORMATS § 2.4), its fields,
 * extension area and signature as they came. Signed by the grantor's account key, so it proves itself: a device checks
 * it against the grantor's key before it keeps one (the locker's seal says only that one of our devices wrote it).
 */
struct AccountStateGrant {
  /** Who granted: the account whose inboxes it opens (`recipient_account_id`). */
  std::string grantor_account_id;
  /** Who it was given to: this account (`sender_account_id`). */
  std::string grantee_account_id;
  /** Unix ms. */
  int64_t issued_at = 0;
  int64_t expires_at = 0;
  /** The grant's extension entries as received (`ext_b64`; empty for none). */
  std::string ext_b64;
  /** The grantor's typed signature, standard base64. */
  std::string signature;

  bool operator==(const AccountStateGrant&) const = default;
};

/** The relay grant version a locker's `grants` rows hold (lib/delivery's kRelayGrantVersion; a row of another is dropped). */
inline constexpr int64_t kAccountStateGrantVersion = 2;

/**
 * One relay of ours as the locker keeps it: its address, the origin and its relay id (`relay:…`, SIGNED_FORMATS § 3.3:
 * the relay proves it with its certificate).
 */
struct AccountStateRelay {
  std::string origin;
  std::string relay_id;

  bool operator==(const AccountStateRelay&) const = default;
};

/**
 * The account's relays (My relays, multi-relay MR006), preferred first, as its card issued at `card_issued_at` lists
 * them: a restored device takes them before its first card, instead of the deployment's default. Empty: none kept.
 */
struct AccountStateRelays {
  std::vector<AccountStateRelay> list;
  int64_t card_issued_at = 0;

  bool operator==(const AccountStateRelays&) const = default;
};

/**
 * Account state the locker keeps beyond the contacts: the groups, the card floors, the grants given to us and our
 * relays.
 */
struct AccountStateExtras {
  std::vector<AccountStateGroup> groups;
  std::vector<AccountCardFloor> card_floors;
  std::vector<AccountStateGrant> grants;
  AccountStateRelays relays;
};

/**
 * JSON of the account state's rows (docs/contracts/ACCOUNT_LOCKER.md § State JSON). Writers keep what they do not
 * know (INVENTORY T7): a row written over one read from the locker (`kept`, the row of the same key) carries back the
 * fields this build does not read, so a newer device's additions survive an older device's publish.
 */
class AccountStateCodec {
public:
  /** One group of the locker's `groups`; `kept` is the row this group was read from, if any. */
  static Object GroupToJson(const AccountStateGroup& group, const Object* kept = nullptr);
  /** nullopt when the group id or the owner is missing. */
  static std::optional<AccountStateGroup> GroupFromJson(const Object& json);

  /** One row of the locker's `grants`: relay grant v2's JSON (`grant_version: 2`, …, `ext_b64`, `signature`). */
  static Object GrantToJson(const AccountStateGrant& grant);
  /**
   * A `grants` row of an untrusted copy: `grant_version` 2 and every field present and within bounds (`max_field`
   * bytes at most), else none. The grant's signature is the reader's to check (its grantor's key).
   */
  static std::optional<AccountStateGrant> GrantFromJson(const Object& json, size_t max_field);

  /**
   * Copy into `written` each field of `kept` that is not in `known` and not already in `written`: what this build
   * does not read goes back as it came.
   */
  static void KeepUnknownFields(Object& written, const Object& kept, std::span<const std::string_view> known);
};

/** `AccountStateCodec::GrantToJson`: the grant's JSON as a relay grant reader takes it. */
inline Object AccountStateGrantToJson(const AccountStateGrant& grant) {
  return AccountStateCodec::GrantToJson(grant);
}

} // namespace pbr
