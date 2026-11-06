#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kGroupIdPrefix = "group:";

enum class MemberRole : uint8_t { Owner = 0, Admin = 1, Member = 2 };

enum class GroupInvitePolicy : uint8_t { Everyone = 0, ContactsOnly = 1, Nobody = 2 };

enum class GroupHistoryMode : uint8_t { Fresh = 0, CopyToForkPoint = 1, UserSelected = 2 };

/**
 * G017: who brings people in and renames. Open (the default): any member; Managed: the owner and admins. Set by
 * the owner, carried in its signed statement (G012).
 */
enum class GroupPreset : uint8_t { Open = 0, Managed = 1 };

enum class InviteStatus : uint8_t { Pending = 0, Accepted = 1, Declined = 2, Expired = 3, Blocked = 4 };

enum class GroupMembershipControlType {
  GroupInvite,
  GroupInviteAccept,
  GroupInviteDecline,
  MemberJoined,
  MemberLeft,
  MemberRemoved,
  OwnerTransferred,
  GroupRenamed,
  GroupForked,
  /** Local-only advisory card; never sent on the wire. */
  GroupOwnerUnreachable,
  /** Owner → members: its newly signed roster, when no roster event carries it (after a leave or a transfer, G012). */
  GroupRosterStatement,
  /** A member checking its roster (after a restore, G013) → the owner and the members it knows. */
  GroupRosterRequest,
  /** A member → the one checking: the latest owner-signed roster it holds, and whether the asker is still on it. */
  GroupRosterReply,
  /** Owner → invitee: its accept came when the group was full (G014); the invitee was not added. */
  GroupInviteRefused,
  /** Owner → members: the owner ended the group for everyone (G016); carries the owner-signed roster with `dissolved`. */
  GroupDissolved,
  /** Owner's device → the owner's other devices (control lane, M021): the same dissolve, so they end the group too. */
  GroupDissolvedSibling,
  /** Owner → members (G017): a member made admin or back to member; carries the owner-signed roster. */
  MemberRoleChanged,
  /** Owner → members (G017): the group set Open or Managed; carries the owner-signed roster. */
  GroupPresetChanged,
  /**
   * A member's or admin's signed roster change (G018: a leave, a removal, ...) from the signer's device to each member
   * (and the account a removal names): `{ group_id, change }`.
   */
  GroupChange,
};

/** G014: a group has at most this many active members (the owner included; pending invites do not count). */
inline constexpr size_t kMaxGroupMembers = 50;
/** The error an accept gets when the group is full (G014): the owner then sends `group_invite_refused`. */
inline constexpr const char kGroupFullError[] = "This group is full (50 members)";
/** `group_invite_refused` reason. */
inline constexpr const char kInviteRefusedGroupFull[] = "group_full";
/** `group_invite_refused` reason (G016): the group was dissolved before the invite could be accepted. */
inline constexpr const char kInviteRefusedGroupDissolved[] = "group_dissolved";
/**
 * `group_invite_refused` reason (G019, G020): the inviter may no longer sign the add — the preset or its role changed,
 * or the invitee was removed since (only the owner or an admin brings them back, G018 R2).
 */
inline constexpr const char kInviteRefusedNotAllowed[] = "not_allowed";

/** G019 / G020: why an invite or a rename is refused here (i18n keys the GUI words). */
inline constexpr const char kGroupInviteNotAllowedError[] = "chat.group.err.invite_not_allowed";
inline constexpr const char kGroupInviteRemovedError[] = "chat.group.err.invite_removed";
inline constexpr const char kGroupAlreadyMemberError[] = "chat.group.err.already_member";
inline constexpr const char kGroupRenameNotAllowedError[] = "chat.group.err.rename_not_allowed";

/**
 * G013: a restored group checks its members before it sends again. `Dissolved` (G016): the owner ended the
 * group; its roster is empty and its chat stays read-only. Terminal, like `Removed`.
 */
enum class GroupSyncState : uint8_t { Active = 0, Checking = 1, Removed = 2, Dissolved = 3 };

/** What the owner sets for the group (G017); who may do what is GroupAllows (GroupPermissions.h). */
struct GroupPolicy {
  GroupPreset preset = GroupPreset::Open;
};

struct GroupRosterMember {
  std::string member_identity;
  std::string contact_id;
  MemberRole role = MemberRole::Member;
  int64_t joined_at = 0;
  /** G019: who brought them in (the signer of the `add`); empty for the group's first members. */
  std::string added_by;
};

struct GroupMetadata {
  std::string group_id;
  std::string owner_identity;
  std::string title;
  uint64_t roster_epoch = 1;
  GroupPolicy policy;
  uint32_t session_epoch = 1;
  /** Bumped by the owner on each rename; a rename applies only when newer (renames keep roster_epoch). */
  uint64_t title_version = 0;
  /** The latest owner-signed roster statement we hold (G012), JSON; empty when none yet. */
  std::string roster_statement_json;
  GroupSyncState sync_state = GroupSyncState::Active;
  /** The open roster check (G013): its request id and when it went out (ms); empty / 0 when none. */
  std::string sync_request_id;
  int64_t sync_requested_at = 0;
};

/** A group invite not accepted within a day is void (the inviter stamps expires_at; the invitee's gate drops it). */
inline constexpr int64_t kGroupInviteTtlMs = 24LL * 60 * 60 * 1000;
/** The owner still applies an accept this long after expiry: it may have waited for an owner who was offline. */
inline constexpr int64_t kGroupInviteAcceptGraceMs = 24LL * 60 * 60 * 1000;
/** Accepting an invite past its expiry; also the localization key of what the user sees. */
inline constexpr const char kGroupInviteExpiredError[] = "chat.group.invite_expired";

/** G022: an invite's `context` when it was made during a call (its `call_id` names the call). */
inline constexpr const char kGroupInviteContextCall[] = "call";
/** G022: the stored text of such a group's first line (the transcript says it in the user's language). */
inline constexpr const char kGroupMadeInCallLine[] = "Group made during a call";

struct GroupInvitePayload {
  std::string group_id;
  std::string group_title;
  std::string inviter_identity;
  std::string invitee_identity;
  std::string invite_nonce;
  uint64_t roster_epoch = 0;
  std::optional<int64_t> expires_at;
  MemberRole actor_role = MemberRole::Owner;
  /**
   * G019: the group's owner as the inviter holds it (the newcomer checks the owner's statement against it); empty
   * from an inviter that is the owner. Additive.
   */
  std::string owner_identity;
  /** G019 3: how many are in the group now — the invite shows the count, never the members. Additive; 0: not said. */
  uint64_t member_count = 0;
  /** G022: `kGroupInviteContextCall` for a group made during the call `call_id`; empty for any other invite. */
  std::string context;
  std::string call_id;

  /** Made during a call, and names it. */
  bool MadeInCall() const { return context == kGroupInviteContextCall && !call_id.empty(); }
  /** The owner as the invite names it: the inviter when it says none. */
  const std::string& Owner() const { return owner_identity.empty() ? inviter_identity : owner_identity; }
};

struct GroupForkPayload {
  std::string source_group_id;
  std::string new_group_id;
  std::string new_group_title;
  std::vector<std::string> selected_identities;
  GroupHistoryMode history_mode = GroupHistoryMode::Fresh;
  std::optional<std::string> fork_message_id;
  std::string actor_identity;
  uint64_t roster_epoch = 0;
};

struct PendingGroupInvite {
  std::string invite_nonce;
  std::string group_id;
  std::string group_title;
  std::string inviter_identity;
  std::string invitee_identity;
  uint64_t roster_epoch = 1;
  InviteStatus status = InviteStatus::Pending;
  std::optional<int64_t> expires_at;
  int64_t created_at = 0;
  /** This device issued the invite (we own the group, or invited as a member, G019); false for one we received. */
  bool outbound = false;
  /** G019: the group's owner as the invite named it (a member's invite); empty: the inviter. */
  std::string owner_identity;

  const std::string& Owner() const { return owner_identity.empty() ? inviter_identity : owner_identity; }
};

std::string GenerateGroupId();
std::string MemberRoleToString(MemberRole role);
MemberRole MemberRoleFromString(const std::string& value);
GroupInvitePolicy GroupInvitePolicyFromString(const std::string& value);
std::string GroupMembershipControlTypeToWire(GroupMembershipControlType type);
std::optional<GroupMembershipControlType> GroupMembershipControlTypeFromWire(const std::string& value);
/** The roster sync controls (G012/G013): kept out of transcripts and unread counts. */
bool IsGroupRosterSyncControl(GroupMembershipControlType type);
std::string GroupSyncStateToString(GroupSyncState state);
GroupSyncState GroupSyncStateFromString(const std::string& value);
std::string GroupPresetToString(GroupPreset preset);
/** Unknown values read as Open. */
GroupPreset GroupPresetFromString(const std::string& value);

} // namespace pbr
