#include "lib/messaging/GroupTypes.h"

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
  case GroupMembershipControlType::GroupRosterStatement:
    return "group_roster_statement";
  case GroupMembershipControlType::GroupRosterRequest:
    return "group_roster_request";
  case GroupMembershipControlType::GroupRosterReply:
    return "group_roster_reply";
  case GroupMembershipControlType::GroupInviteRefused:
    return "group_invite_refused";
  case GroupMembershipControlType::GroupDissolved:
    return "group_dissolved";
  case GroupMembershipControlType::GroupDissolvedSibling:
    return "group_dissolved_sibling";
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
  if (value == "group_invite_refused") {
    return GroupMembershipControlType::GroupInviteRefused;
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
  if (value == "group_roster_statement") {
    return GroupMembershipControlType::GroupRosterStatement;
  }
  if (value == "group_roster_request") {
    return GroupMembershipControlType::GroupRosterRequest;
  }
  if (value == "group_roster_reply") {
    return GroupMembershipControlType::GroupRosterReply;
  }
  if (value == "group_dissolved") {
    return GroupMembershipControlType::GroupDissolved;
  }
  if (value == "group_dissolved_sibling") {
    return GroupMembershipControlType::GroupDissolvedSibling;
  }
  return std::nullopt;
}

bool IsGroupRosterSyncControl(const GroupMembershipControlType type) {
  return type == GroupMembershipControlType::GroupRosterStatement ||
         type == GroupMembershipControlType::GroupRosterRequest || type == GroupMembershipControlType::GroupRosterReply;
}

std::string GroupSyncStateToString(const GroupSyncState state) {
  switch (state) {
  case GroupSyncState::Checking:
    return "checking";
  case GroupSyncState::Removed:
    return "removed";
  case GroupSyncState::Dissolved:
    return "dissolved";
  case GroupSyncState::Active:
    break;
  }
  return "active";
}

GroupSyncState GroupSyncStateFromString(const std::string& value) {
  if (value == "checking") {
    return GroupSyncState::Checking;
  }
  if (value == "removed") {
    return GroupSyncState::Removed;
  }
  if (value == "dissolved") {
    return GroupSyncState::Dissolved;
  }
  return GroupSyncState::Active;
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
