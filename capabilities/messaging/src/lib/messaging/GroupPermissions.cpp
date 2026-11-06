#include "lib/messaging/GroupPermissions.h"

namespace pbr {

bool GroupAllows(const GroupAction action, const MemberRole actor, const GroupPreset preset, const MemberRole target) {
  const bool owner = actor == MemberRole::Owner;
  const bool keeps_order = owner || actor == MemberRole::Admin;
  switch (action) {
    case GroupAction::Send:
      return true;
    case GroupAction::Invite:
    case GroupAction::MakeInviteLink:
    case GroupAction::BringCallGuest:
    case GroupAction::Rename:
      // Open: any member brings people in and renames; Managed: the owner and admins.
      return preset == GroupPreset::Open || keeps_order;
    case GroupAction::Remove:
      // The owner removes anyone but itself; an admin removes members, not admins or the owner.
      return target != MemberRole::Owner && (owner || (actor == MemberRole::Admin && target == MemberRole::Member));
    case GroupAction::Readmit:
    case GroupAction::RevokeInviteLink:
    case GroupAction::RemoveCallGuest:
    case GroupAction::MentionEveryone:
      return keeps_order;
    case GroupAction::Leave:
      return !owner;
    case GroupAction::SetRole:
      return owner && target != MemberRole::Owner;
    case GroupAction::SetPreset:
    case GroupAction::Transfer:
    case GroupAction::Dissolve:
      return owner;
  }
  return false;
}

} // namespace pbr
