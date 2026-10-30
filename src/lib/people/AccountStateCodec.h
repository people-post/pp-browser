#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
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

/** Account state the locker keeps beyond the contacts: the groups and the card floors. */
struct AccountStateExtras {
  std::vector<AccountStateGroup> groups;
  std::vector<AccountCardFloor> card_floors;
};

/** JSON for one group of the account state, as the account locker's `groups` carry it. */
class AccountStateCodec {
public:
  static Object GroupToJson(const AccountStateGroup& group);
  /** nullopt when the group id or the owner is missing. */
  static std::optional<AccountStateGroup> GroupFromJson(const Object& json);
};

} // namespace pbr
