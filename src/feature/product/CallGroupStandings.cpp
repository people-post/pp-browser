#include "feature/product/CallGroupStandings.h"

#include "lib/messaging/GroupPermissions.h"

namespace pbr {

CallGroupStanding CallStandingOfMember(const MemberRole role, const GroupPreset preset) {
  CallGroupStanding standing;
  standing.member = true;
  standing.may_bring_guest = GroupAllows(GroupAction::BringCallGuest, role, preset);
  standing.may_remove_guest = GroupAllows(GroupAction::RemoveCallGuest, role, preset);
  // The membership workflow sends group invites from the owner only until members invite (group-chat o4); then
  // this is GroupAllows(Invite, …) alone.
  standing.may_invite_to_group = role == MemberRole::Owner && GroupAllows(GroupAction::Invite, role, preset);
  return standing;
}

std::optional<CallGroupStanding> CallStandingFromRoster(const IGroupRosterStore& rosters, const std::string& group_id,
                                                        const std::string& account) {
  auto members = rosters.ListMembers(group_id);
  if (!members) {
    return std::nullopt;
  }
  auto metadata = rosters.LoadMetadata(group_id);
  if (members->empty()) {
    // A group the owner dissolved (G016) lists nobody; any other empty roster is one this device does not hold.
    if (metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Dissolved) {
      return CallGroupStanding{};
    }
    return std::nullopt;
  }
  const bool has_metadata = metadata && *metadata;
  const GroupPreset preset = has_metadata ? (*metadata)->policy.preset : GroupPreset::Open;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == account) {
      const bool owner = has_metadata && (*metadata)->owner_identity == account;
      return CallStandingOfMember(owner ? MemberRole::Owner : member.role, preset);
    }
  }
  return CallGroupStanding{};
}

} // namespace pbr
