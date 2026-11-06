#pragma once

#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "lib/people/AccountCard.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** The newest account card this device holds for an account (R6), if any. */
using GroupCardLookup = std::function<std::optional<AccountCard>(const std::string& account)>;

/** How a change's signature check came out (CheckChanges). */
enum class GroupChangeProof : uint8_t {
  Proved,
  /** Fails CheckShape. */
  Malformed,
  /** No card this device has lists its device: it may verify once the card arrives. */
  NoCard,
  /** A card lists its device and the signature does not verify for it (forged or damaged). */
  BadSignature,
};

/** A change after its signature check (CheckChanges): the rules below run on these, crypto-free. */
struct CheckedGroupChange {
  GroupChange change;
  std::string id;
  GroupChangeProof proof = GroupChangeProof::NoCard;
  /** Its device is on the signer's newest card (G018 R6). */
  bool device_current = false;
};

/**
 * Verify each change (group_change::Verify) against its signer's cards: the one `cards` holds and every
 * `signer_card` the changes carry, each proving itself for the signer. The newest of them (highest `issued_at`;
 * ties to the lower signature) decides R6. Copies sharing an id collapse to one (a verified copy first).
 * Sorted by id.
 */
std::vector<CheckedGroupChange> CheckChanges(const std::vector<GroupChange>& changes, const GroupCardLookup& cards,
                                             int64_t now_ms);

/** What became of one change. */
enum class GroupChangeStatus : uint8_t {
  /** It counts. */
  Applied,
  /** A rename that lost (R4). */
  Superseded,
  /** An add a `leave` or `remove` names (R1). */
  Undone,
  /** It does not count; see the reason. */
  Void,
  /** Not held: another group, already folded, malformed, a forged signature, or a dissolved group. */
  Dropped,
};

enum class GroupChangeReason : uint8_t {
  None,
  // Dropped.
  OtherGroup,
  Folded,
  /** Made against a statement older than the latest's change floor (G018: a change that old is lost). */
  TooOld,
  Dissolved,
  Malformed,
  BadSignature,
  // Void, and it may count once more arrives (the owner does not fold these).
  NoCard,
  /** Its signer is in neither the statement nor any add that stands. */
  SignerNotJoined,
  /** A `link_revoke` of a link nobody holds. */
  NoSuchLink,
  // Void by the rules.
  /** R6: its device is off the signer's newest card, and nothing folded or kept it. */
  DeviceOffCard,
  /** R3: its signer left or was removed, and nothing kept it. */
  SignerOut,
  /** R3: its signer may not make it under the latest statement (role, preset, target). */
  NotAllowed,
  /** R2: a member's add of someone a `remove` took out. */
  RemovedMember,
  /** R2: an add of someone removed that is not an owner's or admin's re-admit naming the removal. */
  NotReadmitted,
  /** R5: past 50 members. */
  GroupFull,
  /** A `link_add` for a link id already in the roster. */
  DuplicateLink,
  // Undone / superseded.
  NamedByLeave,
  NamedByRemove,
  /** A rename at or below the statement's title version. */
  OlderTitle,
  /** R4: another rename won. */
  LostRename,
};

struct GroupChangeVerdict {
  std::string id;
  GroupChangeKind kind = GroupChangeKind::Add;
  std::string signer;
  /** The change's `base_epoch` (what a fold records, FoldInto). */
  uint64_t base_epoch = 0;
  GroupChangeStatus status = GroupChangeStatus::Void;
  GroupChangeReason reason = GroupChangeReason::None;
};

struct GroupRosterViewMember {
  std::string identity;
  MemberRole role = MemberRole::Member;
  int64_t joined_at = 0;
  std::string added_by;
  /** The add that brought them in; empty when the statement lists them. */
  std::string added_in;
};

/**
 * How many statements a change may lag behind (G018, the change floor): folding at epoch e raises the floor to
 * e − kChangeFloorLag. Each statement is one owner act (an add, a removal, a role, a fold), so 32 is weeks of a busy
 * group's life — far longer than a change spends in flight from a device that was offline — while `folded`, which
 * rides every statement, names only the changes of the last 32 statements.
 */
inline constexpr uint64_t kChangeFloorLag = 32;

/** The roster: the latest statement plus every unfolded change, by R1–R6 (G018). */
struct GroupRosterView {
  std::string group_id;
  uint64_t statement_epoch = 0;
  std::string owner_identity;
  GroupPreset preset = GroupPreset::Open;
  bool dissolved = false;
  /** Sorted by identity. */
  std::vector<GroupRosterViewMember> members;
  /** Sorted by identity: removed and not back. */
  std::vector<GroupRemovedMember> removed;
  /** Live links: in the roster, not revoked, not expired, their maker in the group. Sorted by link id. */
  std::vector<GroupInviteLink> links;
  std::string title;
  uint64_t title_version = 0;
  /** One per distinct change id given, sorted by id. */
  std::vector<GroupChangeVerdict> verdicts;

  bool Lists(const std::string& identity) const;
  const GroupRosterViewMember* Find(const std::string& identity) const;
  const GroupChangeVerdict* Verdict(const std::string& change_id) const;
  /** The ids a device keeps holding (all but Dropped): what `changes_hash` covers. */
  std::vector<std::string> HeldIds() const;
};

/**
 * G018: the roster from `latest` (the newest owner statement this device holds, already verified) and `changes`.
 * A function of the set: the same statement and changes give the same view in any order. A dissolved statement
 * gives an empty roster.
 */
GroupRosterView ComputeRoster(const GroupRosterStatement& latest, const std::vector<GroupChange>& changes,
                              const GroupCardLookup& cards, int64_t now_ms);
/** The same on changes already checked (CheckChanges); `now_ms` judges link expiry. */
GroupRosterView ComputeRosterChecked(const GroupRosterStatement& latest, const std::vector<CheckedGroupChange>& changes,
                                     int64_t now_ms);

/** Whether the owner folds a change with this verdict (G018 item 4): all held changes but those that may yet count. */
bool FoldsIn(const GroupChangeVerdict& verdict);

/**
 * The fold's bookkeeping on `next` (its `roster_epoch` already set): `folded` = `current.folded` plus every change of
 * `view` that FoldsIn (its base epoch at most `current`'s epoch: no change is made against a statement not yet
 * signed), the change floor raised to `next.roster_epoch` - kChangeFloorLag (never lowered), and `folded` trimmed to
 * the floor. `view` is ComputeRoster over `current`.
 */
void FoldInto(const GroupRosterStatement& current, const GroupRosterView& view, GroupRosterStatement& next);

/**
 * The owner's next statement (unsigned; sign with group_roster_statement::SignStatement): `view`'s members, roles,
 * added_by, removed, live links and title, at `current`'s epoch + 1, with `current`'s preset and owner chain, and
 * the fold's bookkeeping (FoldInto). `view` is ComputeRoster over `current`.
 */
GroupRosterStatement FoldChanges(const GroupRosterStatement& current, const GroupRosterView& view);

} // namespace pbr
