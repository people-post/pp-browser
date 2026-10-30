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

enum class GroupHistoryVisibility : uint8_t { Full = 0, SinceJoin = 1 };

enum class GroupInvitePolicyWire : uint8_t { OwnerOnly = 0, Admins = 1, AnyMember = 2 };

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
};

/** G013: a restored group checks its members before it sends again. */
enum class GroupSyncState : uint8_t { Active = 0, Checking = 1, Removed = 2 };

enum GroupPermission : uint32_t {
  kPermSend = 1u << 0,
  kPermInvite = 1u << 1,
  kPermRemove = 1u << 2,
  kPermRename = 1u << 3,
  kPermTransferOwner = 1u << 4,
  kPermFork = 1u << 5,
  kPermLeave = 1u << 6,
};

struct GroupPolicy {
  GroupInvitePolicyWire invite_policy = GroupInvitePolicyWire::OwnerOnly;
  GroupHistoryVisibility history_visibility = GroupHistoryVisibility::Full;
};

struct GroupRosterMember {
  std::string member_identity;
  std::string contact_id;
  MemberRole role = MemberRole::Member;
  int64_t joined_at = 0;
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

struct GroupInvitePayload {
  std::string group_id;
  std::string group_title;
  std::string inviter_identity;
  std::string invitee_identity;
  std::string invite_nonce;
  uint64_t roster_epoch = 0;
  std::optional<int64_t> expires_at;
  MemberRole actor_role = MemberRole::Owner;
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
  /** This device issued the invite (we own the group); false for one we received. */
  bool outbound = false;
};

std::string GenerateGroupId();
std::string MemberRoleToString(MemberRole role);
MemberRole MemberRoleFromString(const std::string& value);
std::string GroupInvitePolicyToString(GroupInvitePolicy policy);
GroupInvitePolicy GroupInvitePolicyFromString(const std::string& value);
std::string GroupMembershipControlTypeToWire(GroupMembershipControlType type);
std::optional<GroupMembershipControlType> GroupMembershipControlTypeFromWire(const std::string& value);
/** The roster sync controls (G012/G013): kept out of transcripts and unread counts. */
bool IsGroupRosterSyncControl(GroupMembershipControlType type);
std::string GroupSyncStateToString(GroupSyncState state);
GroupSyncState GroupSyncStateFromString(const std::string& value);
uint32_t DefaultPermissionsForRole(MemberRole role);
bool RoleHasPermission(MemberRole role, GroupPermission permission);

} // namespace pbr
