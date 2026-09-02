#include "domain/messaging/GroupMembershipApply.h"

#include "common/Utilities.h"

#include <algorithm>
#include "common/PbrCompat.h"

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
  if (invite.invitee_identity != member_identity) {
    return Error("Invite accept sender does not match invitee");
  }

  // Idempotent re-delivery of accept: ensure member is active and invite marked accepted.
  if (invite.status == InviteStatus::Accepted) {
    GroupRosterMember member;
    member.member_identity = member_identity;
    member.role = MemberRole::Member;
    member.joined_at = util::NowUnixMs();
    return roster.UpsertMember(invite.group_id, member);
  }
  if (invite.status != InviteStatus::Pending) {
    return Error("Invite is not pending");
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

namespace {

Roe<GroupMetadata> LoadMetadataRequired(GroupRosterStore& roster, const std::string& group_id) {
  auto metadata = roster.LoadMetadata(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return Error("Group metadata not found");
  }
  return **metadata;
}

Roe<void> RequireNewerEpoch(const GroupMetadata& metadata, const uint64_t inbound_epoch) {
  if (inbound_epoch <= metadata.roster_epoch) {
    return Error("Stale roster_epoch");
  }
  return {};
}

} // namespace

Roe<void> ApplyMemberJoinedToRoster(GroupRosterStore& roster, const GroupMembershipCodec::MemberJoinedPayload& payload,
                                    const std::string& actor_identity) {
  auto metadata = LoadMetadataRequired(roster, payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity != actor_identity) {
    return Error("member_joined rejected: actor is not the group owner");
  }
  if (auto epoch = RequireNewerEpoch(*metadata, payload.roster_epoch); !epoch) {
    return epoch.error();
  }
  if (payload.member_identity.empty()) {
    return Error("member_joined missing member_identity");
  }

  const int64_t now = util::NowUnixMs();
  auto upsert_one = [&](const std::string& identity, MemberRole role) -> Roe<void> {
    if (identity.empty()) {
      return {};
    }
    GroupRosterMember member;
    member.member_identity = identity;
    member.role = role;
    member.joined_at = now;
    return roster.UpsertMember(payload.group_id, member);
  };

  if (auto upserted = upsert_one(payload.member_identity, payload.role); !upserted) {
    return upserted.error();
  }
  for (const GroupMembershipCodec::MemberJoinedEntry& entry : payload.members) {
    if (entry.member_identity == payload.member_identity) {
      continue;
    }
    if (auto upserted = upsert_one(entry.member_identity, entry.role); !upserted) {
      return upserted.error();
    }
  }
  GroupMetadata updated = *metadata;
  updated.roster_epoch = payload.roster_epoch;
  return roster.UpsertMetadata(updated);
}

Roe<void> ApplyOwnerTransferredToRoster(GroupRosterStore& roster,
                                        const GroupMembershipCodec::OwnerTransferredPayload& payload,
                                        const std::string& actor_identity) {
  auto metadata = LoadMetadataRequired(roster, payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity != actor_identity) {
    return Error("Transfer rejected: actor is not the group owner");
  }
  if (auto epoch = RequireNewerEpoch(*metadata, payload.roster_epoch); !epoch) {
    return epoch.error();
  }
  if (payload.new_owner_identity.empty() || payload.new_owner_identity == actor_identity) {
    return Error("Invalid new owner");
  }
  auto is_member = roster.IsMember(payload.group_id, payload.new_owner_identity);
  if (!is_member) {
    return is_member.error();
  }
  if (!*is_member) {
    return Error("New owner is not a roster member");
  }

  GroupMetadata updated = *metadata;
  updated.owner_identity = payload.new_owner_identity;
  updated.roster_epoch = payload.roster_epoch;
  if (auto saved = roster.UpsertMetadata(updated); !saved) {
    return saved.error();
  }

  GroupRosterMember new_owner;
  new_owner.member_identity = payload.new_owner_identity;
  new_owner.role = MemberRole::Owner;
  new_owner.joined_at = util::NowUnixMs();
  if (auto upserted = roster.UpsertMember(payload.group_id, new_owner); !upserted) {
    return upserted.error();
  }

  if (payload.leave_previous) {
    if (auto removed = roster.RemoveMember(payload.group_id, actor_identity); !removed) {
      return removed.error();
    }
  } else {
    GroupRosterMember previous;
    previous.member_identity = actor_identity;
    previous.role = MemberRole::Member;
    previous.joined_at = util::NowUnixMs();
    (void)roster.UpsertMember(payload.group_id, previous);
  }
  return {};
}

Roe<void> ApplyMemberLeftToRoster(GroupRosterStore& roster, const GroupMembershipCodec::MemberLeftPayload& payload,
                                  const std::string& actor_identity) {
  if (payload.member_identity != actor_identity) {
    return Error("member_left actor must match member_identity");
  }
  auto metadata = LoadMetadataRequired(roster, payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity == actor_identity) {
    return Error("Owner cannot leave without transfer");
  }
  if (auto epoch = RequireNewerEpoch(*metadata, payload.roster_epoch); !epoch) {
    return epoch.error();
  }
  if (auto removed = roster.RemoveMember(payload.group_id, payload.member_identity); !removed) {
    return removed.error();
  }
  GroupMetadata updated = *metadata;
  updated.roster_epoch = payload.roster_epoch;
  return roster.UpsertMetadata(updated);
}

Roe<void> ApplyMemberRemovedToRoster(GroupRosterStore& roster,
                                     const GroupMembershipCodec::MemberRemovedPayload& payload,
                                     const std::string& actor_identity) {
  auto metadata = LoadMetadataRequired(roster, payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity != actor_identity) {
    return Error("Remove rejected: actor is not the group owner");
  }
  if (payload.member_identity == actor_identity) {
    return Error("Owner cannot remove self");
  }
  if (auto epoch = RequireNewerEpoch(*metadata, payload.roster_epoch); !epoch) {
    return epoch.error();
  }
  if (auto removed = roster.RemoveMember(payload.group_id, payload.member_identity); !removed) {
    return removed.error();
  }
  GroupMetadata updated = *metadata;
  updated.roster_epoch = payload.roster_epoch;
  return roster.UpsertMetadata(updated);
}

} // namespace pbr
