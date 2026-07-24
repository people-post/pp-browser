#include "base/messaging/GroupMembershipApply.h"

#include "common/Utilities.h"

#include <algorithm>

namespace pbr {

Roe<void> ApplyInviteAcceptToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                    const std::string& member_identity) {
  auto pending = roster.LoadPendingInvite(invite_nonce);
  if (!pending) {
    return pending.error();
  }
  if (!pending->has_value()) {
    return Error("Invite not found");
  }
  const PendingGroupInvite& invite = **pending;
  if (invite.status != InviteStatus::Pending) {
    return Error("Invite is not pending");
  }
  if (invite.invitee_identity != member_identity) {
    return Error("Invite accept sender does not match invitee");
  }

  GroupRosterMember member;
  member.member_identity = member_identity;
  member.role = MemberRole::Member;
  member.joined_at = util::NowUnixMs();
  if (auto upserted = roster.UpsertMember(invite.group_id, member); !upserted) {
    return upserted.error();
  }
  if (auto status = roster.UpdateInviteStatus(invite_nonce, InviteStatus::Accepted); !status) {
    return status.error();
  }

  auto metadata = roster.LoadMetadata(invite.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (*metadata) {
    GroupMetadata updated = **metadata;
    updated.roster_epoch = std::max(updated.roster_epoch, invite.roster_epoch) + 1;
    if (auto saved = roster.UpsertMetadata(updated); !saved) {
      return saved.error();
    }
  }
  return {};
}

Roe<void> ApplyInviteDeclineToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                     const std::string& member_identity) {
  auto pending = roster.LoadPendingInvite(invite_nonce);
  if (!pending) {
    return pending.error();
  }
  if (!pending->has_value()) {
    return Error("Invite not found");
  }
  const PendingGroupInvite& invite = **pending;
  if (invite.invitee_identity != member_identity) {
    return Error("Invite decline sender does not match invitee");
  }
  if (invite.status != InviteStatus::Pending) {
    return Error("Invite is not pending");
  }
  return roster.UpdateInviteStatus(invite_nonce, InviteStatus::Declined);
}

} // namespace pbr
