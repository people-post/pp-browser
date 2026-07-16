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
};

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
};

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
  std::string inviter_identity;
  std::string invitee_identity;
  InviteStatus status = InviteStatus::Pending;
  std::optional<int64_t> expires_at;
  int64_t created_at = 0;
};

std::string GenerateGroupId();
std::string MemberRoleToString(MemberRole role);
MemberRole MemberRoleFromString(const std::string& value);
std::string GroupInvitePolicyToString(GroupInvitePolicy policy);
GroupInvitePolicy GroupInvitePolicyFromString(const std::string& value);
std::string GroupMembershipControlTypeToWire(GroupMembershipControlType type);
std::optional<GroupMembershipControlType> GroupMembershipControlTypeFromWire(const std::string& value);
uint32_t DefaultPermissionsForRole(MemberRole role);
bool RoleHasPermission(MemberRole role, GroupPermission permission);

} // namespace pbr
