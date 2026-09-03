#include "domain/messaging/GroupTypes.h"

#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

std::string GenerateGroupId() {
  return std::string(kGroupIdPrefix) + util::GenerateUuid();
}

std::string MemberRoleToString(const MemberRole role) {
  switch (role) {
  case MemberRole::Owner:
    return "owner";
  case MemberRole::Admin:
    return "admin";
  case MemberRole::Member:
    return "member";
  }
  return "member";
}

MemberRole MemberRoleFromString(const std::string& value) {
  if (value == "owner") {
    return MemberRole::Owner;
  }
  if (value == "admin") {
    return MemberRole::Admin;
  }
  return MemberRole::Member;
}

std::string GroupInvitePolicyToString(const GroupInvitePolicy policy) {
  switch (policy) {
  case GroupInvitePolicy::Everyone:
    return "everyone";
  case GroupInvitePolicy::ContactsOnly:
    return "contacts_only";
  case GroupInvitePolicy::Nobody:
    return "nobody";
  }
  return "contacts_only";
}

GroupInvitePolicy GroupInvitePolicyFromString(const std::string& value) {
  if (value == "everyone") {
    return GroupInvitePolicy::Everyone;
  }
  if (value == "nobody") {
    return GroupInvitePolicy::Nobody;
  }
  return GroupInvitePolicy::ContactsOnly;
}

std::string GroupMembershipControlTypeToWire(const GroupMembershipControlType type) {
  switch (type) {
  case GroupMembershipControlType::GroupInvite:
    return "group_invite";
  case GroupMembershipControlType::GroupInviteAccept:
    return "group_invite_accept";
  case GroupMembershipControlType::GroupInviteDecline:
    return "group_invite_decline";
  case GroupMembershipControlType::MemberJoined:
    return "member_joined";
  case GroupMembershipControlType::MemberLeft:
    return "member_left";
  case GroupMembershipControlType::MemberRemoved:
    return "member_removed";
  case GroupMembershipControlType::OwnerTransferred:
    return "owner_transferred";
  case GroupMembershipControlType::GroupRenamed:
    return "group_renamed";
  case GroupMembershipControlType::GroupForked:
    return "group_forked";
  case GroupMembershipControlType::GroupOwnerUnreachable:
    return "group_owner_unreachable";
  }
  return "group_invite";
}

std::optional<GroupMembershipControlType> GroupMembershipControlTypeFromWire(const std::string& value) {
  if (value == "group_invite") {
    return GroupMembershipControlType::GroupInvite;
  }
  if (value == "group_invite_accept") {
    return GroupMembershipControlType::GroupInviteAccept;
  }
  if (value == "group_invite_decline") {
    return GroupMembershipControlType::GroupInviteDecline;
  }
  if (value == "member_joined") {
    return GroupMembershipControlType::MemberJoined;
  }
  if (value == "member_left") {
    return GroupMembershipControlType::MemberLeft;
  }
  if (value == "member_removed") {
    return GroupMembershipControlType::MemberRemoved;
  }
  if (value == "owner_transferred") {
    return GroupMembershipControlType::OwnerTransferred;
  }
  if (value == "group_renamed") {
    return GroupMembershipControlType::GroupRenamed;
  }
  if (value == "group_forked") {
    return GroupMembershipControlType::GroupForked;
  }
  if (value == "group_owner_unreachable") {
    return GroupMembershipControlType::GroupOwnerUnreachable;
  }
  return std::nullopt;
}

uint32_t DefaultPermissionsForRole(const MemberRole role) {
  switch (role) {
  case MemberRole::Owner:
    return kPermSend | kPermInvite | kPermRemove | kPermRename | kPermTransferOwner | kPermFork | kPermLeave;
  case MemberRole::Admin:
    return kPermSend | kPermInvite | kPermRemove | kPermRename | kPermFork | kPermLeave;
  case MemberRole::Member:
    return kPermSend | kPermFork | kPermLeave;
  }
  return kPermSend | kPermLeave;
}

bool RoleHasPermission(const MemberRole role, const GroupPermission permission) {
  return (DefaultPermissionsForRole(role) & static_cast<uint32_t>(permission)) != 0;
}

} // namespace pbr
