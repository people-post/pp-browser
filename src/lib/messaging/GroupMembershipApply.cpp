#include "lib/messaging/GroupMembershipApply.h"

#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/GroupRosterSync.h"

#include "common/Utilities.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

Roe<bool> ApplyInviteAcceptToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                    const std::string& member_identity, const std::string& group_id,
                                    const std::string& local_identity, const int64_t now_ms) {
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
  auto pending = roster.LoadPendingInvite(invite_nonce);
  if (!pending) {
    return pending.error();
  }
  if (!pending->has_value()) {
    return Error("Invite not found");
  }
  const PendingGroupInvite& invite = **pending;
  if (!invite.outbound) {
    return Error("Invite accept for an invite this device did not issue");
  }
  if (invite.invitee_identity != member_identity) {
    return Error("Invite accept sender does not match invitee");
  }
  if (invite.group_id != group_id) {
    return Error("Invite accept names another group");
  }
  auto metadata = roster.LoadMetadata(invite.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!*metadata || local_identity.empty() || (*metadata)->owner_identity != local_identity) {
    return Error("Invite accept for a group this device does not own");
  }
  if ((*metadata)->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);  // G016: the group is over; the caller refuses the accept as it does a full one
  }
  if (invite.status == InviteStatus::Accepted) {
    // Re-delivered (or re-sent) accept: it was applied once. A member removed since, or one who
    // left, needs a new invite — an old accept does not bring them back.
    return false;
  }
  if (invite.status != InviteStatus::Pending) {
    return Error("Invite is not pending");
  }
  if (invite.expires_at && now_ms > *invite.expires_at + kGroupInviteAcceptGraceMs) {
    (void)roster.UpdateInviteStatus(invite_nonce, InviteStatus::Expired);
    return Error("Invite expired");
  }
  // G014: sent while not full, accepted when full — refused (the caller tells the invitee).
  if (auto members = roster.ListMembers(invite.group_id); members && members->size() >= kMaxGroupMembers) {
    (void)roster.UpdateInviteStatus(invite_nonce, InviteStatus::Expired);
    return Error(kGroupFullError);
  }

  GroupRosterMember member;
  member.member_identity = member_identity;
  member.role = MemberRole::Member;
  member.joined_at = now_ms;
  if (auto upserted = roster.UpsertMember(invite.group_id, member); !upserted) {
    return upserted.error();
  }
  GroupMetadata updated = **metadata;
  updated.roster_epoch += 1;  // our own roster's next epoch — not one the accept names
  if (auto saved = roster.UpsertMetadata(updated); !saved) {
    return saved.error();
  }
  // Last: until the invite reads Accepted, a failure above leaves it Pending and a re-delivered
  // accept applies it again (the member upsert is idempotent) instead of being ignored.
  if (auto status = roster.UpdateInviteStatus(invite_nonce, InviteStatus::Accepted); !status) {
    return status.error();
  }
  // A re-invite left other cards out: once in, one of them must not bring back someone who later leaves.
  (void)roster.ExpireOutboundInvitesTo(invite.group_id, member_identity);
  return true;
}

Roe<void> ApplyInviteDeclineToRoster(GroupRosterStore& roster, const std::string& invite_nonce,
                                     const std::string& member_identity, const std::string& group_id) {
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
  auto pending = roster.LoadPendingInvite(invite_nonce);
  if (!pending) {
    return pending.error();
  }
  if (!pending->has_value()) {
    return Error("Invite not found");
  }
  const PendingGroupInvite& invite = **pending;
  if (!invite.outbound || invite.invitee_identity != member_identity || invite.group_id != group_id) {
    return Error("Invite decline does not match an invite this device issued");
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
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
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
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
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
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
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
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
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

Roe<bool> ApplyGroupRenamedToRoster(GroupRosterStore& roster, const GroupMembershipCodec::GroupRenamedPayload& payload,
                                    const std::string& actor_identity) {
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
  auto metadata = LoadMetadataRequired(roster, payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (actor_identity.empty() || metadata->owner_identity != actor_identity) {
    return Error("Rename rejected: actor is not the group owner");
  }
  if (payload.title_version <= metadata->title_version) {
    return false;  // not newer than the title we hold
  }
  GroupMetadata updated = *metadata;
  updated.title = payload.title;
  updated.title_version = payload.title_version;
  if (payload.roster_epoch > updated.roster_epoch) {
    updated.roster_epoch = payload.roster_epoch;
  }
  if (auto saved = roster.UpsertMetadata(updated); !saved) {
    return saved.error();
  }
  return true;
}

Roe<GroupDissolvedApplied> EndGroupInRoster(GroupRosterStore& roster, const std::string& group_id,
                                           const std::string& owner_identity, const uint64_t roster_epoch,
                                           const std::string& statement_json,
                                           const std::vector<PendingGroupSend>& sends) {
  GroupEnding ending;
  ending.owner_identity = owner_identity;
  ending.roster_epoch = roster_epoch;
  ending.statement_json = statement_json;
  ending.sends = sends;
  auto ended = roster.EndGroup(group_id, ending);
  if (!ended) {
    return ended.error();
  }
  GroupDissolvedApplied result;
  if (*ended) {
    result.applied = true;
    result.voided_invites = std::move(**ended);
  }
  return result;
}

namespace {

/** The shared rule; `actor` (the DM's sender) must be the owner it names when given. */
Roe<GroupDissolvedApplied> ApplyDissolved(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                          const std::string& local_identity, const std::string* actor) {
  const auto lock = GroupRosterStore::LockForUpdate();  // read-then-write of the roster
  GroupDissolvedApplied result;
  auto metadata = roster.LoadMetadata(statement.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!*metadata || (*metadata)->sync_state == GroupSyncState::Dissolved) {
    return result;  // not a group we hold, or already over: a late or repeated copy does nothing
  }
  auto member = local_identity.empty() ? Roe<bool>(false) : roster.IsMember(statement.group_id, local_identity);
  if (!member) {
    return member.error();
  }
  if (!*member) {
    return result;  // a group we are not in (left, removed, deleted): not ours to end
  }
  if (!statement.dissolved) {
    return Error("Dissolve rejected: the statement does not say the group ended");
  }
  if (actor && (actor->empty() || statement.owner_identity != *actor)) {
    return Error("Dissolve rejected: actor is not the owner who signed it");
  }
  // The owner we know, or one it handed the group to (the chain): the same check as any owner statement.
  if (auto valid = group_roster_statement::Verify(statement, (*metadata)->owner_identity); !valid) {
    return Error("Dissolve rejected: " + valid.error().message);
  }
  if (statement.roster_epoch < (*metadata)->roster_epoch) {
    return Error("Dissolve rejected: older than the roster we hold");
  }
  // A former owner, or a stale chain: weighed against every transfer we hold and the statement shows.
  std::vector<GroupOwnerTransfer> known = statement.owner_chain;
  if (auto held = group_roster_sync::Held(**metadata)) {
    known.insert(known.end(), held->owner_chain.begin(), held->owner_chain.end());
  }
  if (group_roster_statement::OwnerHandedOn(known, statement)) {
    return Error("Dissolve rejected: its owner had already handed the group on");
  }
  return EndGroupInRoster(roster, statement.group_id, statement.owner_identity, statement.roster_epoch,
                          DumpJson(group_roster_statement::ToJson(statement)));
}

} // namespace

Roe<GroupDissolvedApplied> ApplyDissolvedStatement(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                                   const std::string& local_identity) {
  return ApplyDissolved(roster, statement, local_identity, nullptr);
}

Roe<GroupDissolvedApplied> ApplyGroupDissolvedToRoster(GroupRosterStore& roster, const GroupRosterStatement& statement,
                                                       const std::string& actor_identity,
                                                       const std::string& local_identity) {
  return ApplyDissolved(roster, statement, local_identity, &actor_identity);
}

Roe<std::optional<std::string>> EnsureDissolvedLine(IThreadStore& store, const std::string& group_id,
                                                    const std::string& owner_identity,
                                                    const std::string& sender_identity) {
  auto thread = store.FindGroupThread(group_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return std::optional<std::string>{};  // the chat was deleted: nothing to keep
  }
  auto page = store.GetMessagesPage((*thread)->id, std::nullopt, 500);
  if (!page) {
    return page.error();
  }
  for (const ThreadMessage& m : *page) {
    const auto type = GroupMembershipCodec::ControlTypeFromMessage(m);
    if (type && *type == GroupMembershipControlType::GroupDissolved) {
      return std::optional<std::string>{};
    }
  }
  auto detail = GroupMembershipCodec::EncodeGroupDissolved(group_id, owner_identity);
  if (!detail) {
    return detail.error();
  }
  auto line = GroupMembershipCodec::BuildSystemMessage((*thread)->id, GroupMembershipControlType::GroupDissolved,
                                                       "Group dissolved", *detail, sender_identity);
  if (!line) {
    return line.error();
  }
  auto appended = store.AppendMessage(*line);
  if (!appended) {
    return appended.error();
  }
  return std::optional<std::string>((*thread)->id);
}

} // namespace pbr
