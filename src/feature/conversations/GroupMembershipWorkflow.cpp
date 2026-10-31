#include "feature/conversations/GroupMembershipWorkflow.h"

#include "lib/people/DirectChatTargetFromContact.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/messaging/PairwiseFanoutLogic.h"
#include "lib/messaging/SendRelayOptions.h"
#include "lib/people/ContactJson.h"

#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppRuntime.h"

#include "common/Utilities.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <optional>
#include <vector>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

/**
 * Cards a member_joined may carry for the rest of the roster: about 9-13 KB each, so a large group
 * sends the first ones and its other members are keyed as before (a first message, the directory).
 */
constexpr size_t kRosterCardsBudgetBytes = 128 * 1024;

/** The line an app that does not know `group_invite_refused` shows for it. */
std::string RefusedDisplay(const std::string& reason) {
  return reason == kInviteRefusedGroupDissolved ? "Group invitation refused: the group was dissolved"
                                                : "Group invitation refused: the group is full";
}

} // namespace

GroupMembershipWorkflow::GroupMembershipWorkflow(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                               GroupRosterStore& roster, GroupInviteGate& invite_gate,
                                               MeshDeliveryOrchestrator& mesh_messaging, AccountKeyResolver& account_keys)
    : store_(store), contacts_(contacts), identity_(identity), roster_(roster), invite_gate_(invite_gate),
      mesh_messaging_(mesh_messaging), account_keys_(account_keys) {
  redirectLogger("GroupMembershipWorkflow");
}

void GroupMembershipWorkflow::SetInboundPolicy(const GroupInvitePolicy policy) {
  invite_gate_.SetInboundPolicy(policy);
}

Roe<std::string> GroupMembershipWorkflow::LocalRelayIdentity() const {
  auto id = identity_.Get();
  if (!id) {
    return id.error();
  }
  if (id->account_id.empty()) {
    return Error("Local account identity unavailable");
  }
  return id->account_id;
}

Roe<std::string> GroupMembershipWorkflow::ResolveRelayIdentity(const std::string& contact_id) const {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }
  const DirectChatTarget target = DirectChatTargetFromContact(**contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no Account ID");
  }
  return target.peer_identity_value;
}

Roe<GroupMetadata> GroupMembershipWorkflow::LoadMetadataOrError(const std::string& group_id) const {
  auto metadata = roster_.LoadMetadata(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return Error("Group metadata not found");
  }
  return **metadata;
}

Roe<void> GroupMembershipWorkflow::AppendMembershipSystemEvent(const std::string& thread_id,
                                                              const GroupMembershipControlType type,
                                                              const std::string& text,
                                                              const std::string& detail_json,
                                                              const std::string& sender_identity) {
  auto message =
      GroupMembershipCodec::BuildSystemMessage(thread_id, type, text, detail_json, sender_identity);
  if (!message) {
    return message.error();
  }
  if (auto appended = store_.AppendMessage(*message); !appended) {
    return appended.error();
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::SendInviteDirectMessage(const GroupInvitePayload& invite,
                                                          const std::string& invitee_contact_id) {
  auto contact = contacts_.Get(invitee_contact_id);
  if (!contact || !*contact) {
    return Error("Invitee contact not found");
  }
  const DirectChatTarget direct_target = DirectChatTargetFromContact(**contact, ThreadChannel::E2ePublic);
  auto thread = store_.FindOrCreateDirectThread(direct_target, invitee_contact_id, (*contact)->display_name);
  if (!thread) {
    return thread.error();
  }
  auto detail = GroupMembershipCodec::EncodeInvite(invite);
  if (!detail) {
    return detail.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  // Send as a system ChatPayload on the wire (same shape as group_renamed). Do not attach
  // Accept/Decline locally — those are for the invitee, hydrated on inbound ingest.
  const std::string display = "Group invitation: " + invite.group_title;
  Object payload;
  payload.set("control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupInvite));
  payload.set("detail", *detail);
  const std::string payload_json = DumpJson(payload);
  SendRelayOptions opts;
  opts.generation = "system";
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_json;
  opts.sender_contact_id = *local;
  auto sent = mesh_messaging_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<Thread> GroupMembershipWorkflow::CreateGroup(const std::string& title,
                                                const std::vector<std::string>& member_contact_ids) {
  auto local_identity = LocalRelayIdentity();
  if (!local_identity) {
    return local_identity.error();
  }

  if (member_contact_ids.size() + 1 > kMaxGroupMembers) {
    return Error(kGroupFullError);  // G014: the owner and its members
  }
  const std::string group_id = GenerateGroupId();
  std::vector<std::string> participants;
  for (const std::string& contact_id : member_contact_ids) {
    participants.push_back(contact_id);
  }

  auto thread = store_.FindOrCreateGroupThread(group_id, title, participants);
  if (!thread) {
    return thread.error();
  }

  GroupMetadata metadata;
  metadata.group_id = group_id;
  metadata.owner_identity = *local_identity;
  metadata.title = title;
  metadata.roster_epoch = 1;
  if (auto saved = roster_.UpsertMetadata(metadata); !saved) {
    return saved.error();
  }

  GroupRosterMember owner;
  owner.member_identity = *local_identity;
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  if (auto member = roster_.UpsertMember(group_id, owner); !member) {
    return member.error();
  }

  if (auto signed_roster = SignRoster(group_id); !signed_roster) {
    log().warning << "Roster not signed group=" << group_id << ": " << signed_roster.error().message;
  }

  auto joined_detail = GroupMembershipCodec::EncodeMemberJoined(group_id, *local_identity, MemberRole::Owner, 1);
  if (!joined_detail) {
    return joined_detail.error();
  }
  if (auto event = AppendMembershipSystemEvent(thread->id, GroupMembershipControlType::MemberJoined,
                                               "Owner created the group", *joined_detail, *local_identity);
      !event) {
    return event.error();
  }

  for (const std::string& contact_id : member_contact_ids) {
    if (auto invited = InviteMember(group_id, contact_id); !invited) {
      return invited.error();
    }
  }
  return *thread;
}

Roe<void> GroupMembershipWorkflow::InviteMember(const std::string& group_id, const std::string& invitee_contact_id) {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (*local != metadata->owner_identity) {
    return Error("Only the group owner may invite members");
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);  // G016: no invites into an ended group
  }
  if (auto can = invite_gate_.CanSendInvite(MemberRole::Owner); !can || !*can) {
    return Error("Actor lacks invite permission");
  }
  // G014: the active roster only — pending invites do not count (an accept that comes when full is refused).
  if (auto members = roster_.ListMembers(group_id); members && members->size() >= kMaxGroupMembers) {
    return Error(kGroupFullError);
  }

  auto invitee_identity = ResolveRelayIdentity(invitee_contact_id);
  if (!invitee_identity) {
    return invitee_identity.error();
  }

  GroupInvitePayload invite;
  invite.group_id = group_id;
  invite.group_title = metadata->title;
  invite.inviter_identity = *local;
  invite.invitee_identity = *invitee_identity;
  invite.invite_nonce = util::GenerateUuid();
  invite.roster_epoch = metadata->roster_epoch;
  invite.expires_at = util::NowUnixMs() + kGroupInviteTtlMs;
  invite.actor_role = MemberRole::Owner;

  PendingGroupInvite pending;
  pending.invite_nonce = invite.invite_nonce;
  pending.group_id = group_id;
  pending.group_title = invite.group_title;
  pending.inviter_identity = invite.inviter_identity;
  pending.invitee_identity = invite.invitee_identity;
  pending.roster_epoch = invite.roster_epoch;
  pending.status = InviteStatus::Pending;
  pending.expires_at = invite.expires_at;
  pending.created_at = util::NowUnixMs();
  pending.outbound = true;
  if (auto recorded = invite_gate_.RecordPendingInvite(pending); !recorded) {
    return recorded.error();
  }
  // Pending only — invitee is not an encrypt target until owner commits member_joined.
  return SendInviteDirectMessage(invite, invitee_contact_id);
}

Roe<void> GroupMembershipWorkflow::RefuseInvite(const std::string& group_id, const std::string& invite_nonce,
                                               const std::string& invitee_identity, const std::string& reason) {
  const std::string why = reason.empty() ? std::string(kInviteRefusedGroupFull) : reason;
  auto detail = GroupMembershipCodec::EncodeInviteRefused(invite_nonce, group_id, why);
  if (!detail) {
    return detail.error();
  }
  return SendMembershipDirectMessage(invitee_identity, GroupMembershipControlType::GroupInviteRefused, *detail,
                                     RefusedDisplay(why));
}

Roe<void> GroupMembershipWorkflow::SendInviteResponseDirectMessage(const std::string& inviter_identity,
                                                                  const std::string& invite_nonce,
                                                                  const std::string& group_id,
                                                                  const GroupMembershipControlType response_type) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = inviter_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = inviter_identity;
  if (auto contact = contacts_.FindByIdentity(inviter_identity, mesh::ContactIdKind::Account)) {
    if (*contact) {
      contact_id = (*contact)->id;
      dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (dm_title.empty()) {
        dm_title = inviter_identity;
      }
    }
  }

  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
  if (!thread) {
    return thread.error();
  }
  auto detail = GroupMembershipCodec::EncodeInviteResponse(invite_nonce, group_id);
  if (!detail) {
    return detail.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const bool accepted = response_type == GroupMembershipControlType::GroupInviteAccept;
  const std::string display = accepted ? "Accepted group invitation" : "Declined group invitation";
  Object payload;
  payload.set("control_type", GroupMembershipControlTypeToWire(response_type));
  payload.set("detail", *detail);
  const std::string payload_json = DumpJson(payload);
  SendRelayOptions opts;
  opts.generation = "system";
  opts.update_preview = false;
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_json;
  opts.sender_contact_id = *local;
  auto sent = mesh_messaging_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<Thread> GroupMembershipWorkflow::AcceptInvite(const std::string& invite_nonce) {
  auto pending = roster_.LoadPendingInvite(invite_nonce);
  if (!pending || !pending->has_value() || (*pending)->outbound) {
    return Error("Invite not found");
  }
  if ((*pending)->status != InviteStatus::Pending) {
    return Error("Invite is not pending");
  }
  if ((*pending)->expires_at && util::NowUnixMs() > *(*pending)->expires_at) {
    (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Expired);
    return Error(kGroupInviteExpiredError);
  }

  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (*local != (*pending)->invitee_identity) {
    return Error("Invite not addressed to local identity");
  }

  if (IsDissolved((*pending)->group_id)) {
    (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Expired);  // G016: an invite to an ended group is void
    return Error(group_roster_sync::kGroupDissolvedError);
  }

  const std::string title =
      (*pending)->group_title.empty() ? std::string("Group chat") : (*pending)->group_title;
  const uint64_t roster_epoch = (*pending)->roster_epoch == 0 ? 1 : (*pending)->roster_epoch;

  GroupMetadata metadata;
  metadata.group_id = (*pending)->group_id;
  metadata.owner_identity = (*pending)->inviter_identity;
  metadata.title = title;
  metadata.roster_epoch = roster_epoch;
  if (auto saved = roster_.UpsertMetadata(metadata); !saved) {
    return saved.error();
  }

  auto thread = store_.FindOrCreateGroupThread((*pending)->group_id, title, {});
  if (!thread) {
    return thread.error();
  }

  // Seed both sides of the v1 create-invite pair so SendGroupMessage can fan out.
  // (Owner already has self; invitee must also know the owner.)
  GroupRosterMember owner;
  owner.member_identity = (*pending)->inviter_identity;
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  if (auto upsert_owner = roster_.UpsertMember((*pending)->group_id, owner); !upsert_owner) {
    return upsert_owner.error();
  }

  GroupRosterMember member;
  member.member_identity = *local;
  member.role = MemberRole::Member;
  member.joined_at = util::NowUnixMs();
  if (auto upsert = roster_.UpsertMember((*pending)->group_id, member); !upsert) {
    return upsert.error();
  }
  (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Accepted);
  (void)roster_.UpsertGroupTarget((*pending)->group_id, thread->id, 1, 1);

  auto detail =
      GroupMembershipCodec::EncodeMemberJoined((*pending)->group_id, *local, MemberRole::Member, roster_epoch);
  if (!detail) {
    return detail.error();
  }
  if (auto event = AppendMembershipSystemEvent(thread->id, GroupMembershipControlType::MemberJoined,
                                               "You joined the group", *detail, *local);
      !event) {
    return event.error();
  }

  if (auto sent = SendInviteResponseDirectMessage((*pending)->inviter_identity, invite_nonce, (*pending)->group_id,
                                                  GroupMembershipControlType::GroupInviteAccept);
      !sent) {
    return sent.error();
  }
  (void)ResolveInviteCard((*pending)->inviter_identity, invite_nonce, InviteStatus::Accepted,
                          "You joined " + title);
  return *thread;
}

Roe<void> GroupMembershipWorkflow::DeclineInvite(const std::string& invite_nonce) {
  auto pending = roster_.LoadPendingInvite(invite_nonce);
  if (!pending || !pending->has_value() || (*pending)->outbound) {
    return Error("Invite not found");
  }
  if ((*pending)->status != InviteStatus::Pending) {
    return Error("Invite is not pending");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (*local != (*pending)->invitee_identity) {
    return Error("Invite not addressed to local identity");
  }
  if (auto status = roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Declined); !status) {
    return status.error();
  }
  // Best-effort notify inviter; local decline already recorded.
  (void)SendInviteResponseDirectMessage((*pending)->inviter_identity, invite_nonce, (*pending)->group_id,
                                        GroupMembershipControlType::GroupInviteDecline);
  const std::string title =
      (*pending)->group_title.empty() ? std::string("group invitation") : (*pending)->group_title;
  (void)ResolveInviteCard((*pending)->inviter_identity, invite_nonce, InviteStatus::Declined,
                          "You declined " + title);
  return {};
}

Roe<void> GroupMembershipWorkflow::DismissLocalGroup(const std::string& group_id) {
  if (group_id.empty()) {
    return Error("group_id required");
  }
  auto local = LocalRelayIdentity();
  if (local) {
    (void)roster_.RemoveMember(group_id, *local);
  }
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

Roe<void> GroupMembershipWorkflow::ResolveInviteCard(const std::string& inviter_identity,
                                                    const std::string& invite_nonce, const InviteStatus status,
                                                    const std::string& status_text) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = inviter_identity;
  direct_target.channel = ThreadChannel::E2ePublic;
  auto thread = store_.FindDirectThread(direct_target);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    // Invite may have landed on e2e channel in tests; try e2e as well.
    direct_target.channel = ThreadChannel::E2e;
    thread = store_.FindDirectThread(direct_target);
    if (!thread || !*thread) {
      return {};
    }
  }
  auto messages = store_.GetMessagesPage((*thread)->id, std::nullopt, 500);
  if (!messages) {
    return messages.error();
  }
  for (ThreadMessage& message : *messages) {
    auto invite = GroupMembershipCodec::DecodeInviteFromMessage(message);
    if (!invite || invite->invite_nonce != invite_nonce) {
      continue;
    }
    GroupMembershipCodec::ApplyInviteResolution(message, status, status_text);
    (void)store_.UpdateMessage(message);
    return {};
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::SendMembershipDirectMessage(const std::string& peer_identity,
                                                              const GroupMembershipControlType control_type,
                                                              const std::string& detail_json,
                                                              const std::string& display,
                                                              const std::optional<std::string>& stored_detail_json) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, mesh::ContactIdKind::Account)) {
    if (*contact) {
      contact_id = (*contact)->id;
      dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (dm_title.empty()) {
        dm_title = peer_identity;
      }
    }
  }

  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
  if (!thread) {
    return thread.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto payload_with = [control_type](const std::string& detail) {
    Object payload;
    payload.set("control_type", GroupMembershipControlTypeToWire(control_type));
    payload.set("detail", detail);
    return DumpJson(payload);
  };
  SendRelayOptions opts;
  opts.generation = "system";
  opts.update_preview = false;
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_with(detail_json);
  if (stored_detail_json) {
    opts.stored_payload_json = payload_with(*stored_detail_json);
  }
  opts.sender_contact_id = *local;
  auto sent = mesh_messaging_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::FanOutMembershipEvent(const std::string& group_id,
                                                         const GroupMembershipControlType control_type,
                                                         const std::string& detail_json, const std::string& display,
                                                         const std::string& skip_identity,
                                                         const std::optional<std::string>& also_notify_identity) {
  auto members = roster_.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  std::vector<std::string> identities;
  identities.reserve(members->size());
  for (const GroupRosterMember& member : *members) {
    identities.push_back(member.member_identity);
  }
  const auto targets = SelectPairwiseFanoutTargets(identities, skip_identity);
  const auto result = FanOutPairwise(targets, PairwiseFanoutMode::FailFast,
                                     [&](const std::string& identity) {
                                       return SendMembershipDirectMessage(identity, control_type, detail_json,
                                                                         display);
                                     });
  if (result.first_error) {
    return *result.first_error;
  }
  if (also_notify_identity && !also_notify_identity->empty() && *also_notify_identity != skip_identity) {
    bool already = false;
    for (const std::string& id : targets) {
      if (id == *also_notify_identity) {
        already = true;
        break;
      }
    }
    if (!already) {
      (void)SendMembershipDirectMessage(*also_notify_identity, control_type, detail_json, display);
    }
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::RemoveMemberInternal(const std::string& group_id,
                                                        const std::string& member_identity) {
  auto update = GroupRosterStore::LockForUpdate();  // the epoch read below and its apply, as one
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (*local != metadata->owner_identity) {
    return Error("Only the group owner may remove members");
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);
  }
  if (member_identity == *local) {
    return Error("Owner cannot remove self — transfer ownership or leave");
  }

  // The same change every member applies from the event (one rule for both replicas).
  const GroupMembershipCodec::MemberRemovedPayload removal{group_id, member_identity, metadata->roster_epoch + 1};
  if (auto applied = ApplyMemberRemovedToRoster(roster_, removal, *local); !applied) {
    return applied.error();
  }
  metadata->roster_epoch = removal.roster_epoch;
  update.unlock();  // not across the sends below
  // A removed member must not keep showing as "invited, waiting".
  (void)roster_.ExpireOutboundInvitesTo(group_id, member_identity);
  // The removed member reads nothing that follows: no copy is made for it (every copy is pairwise, G011).
  ClearMemberUnreachable(group_id, member_identity);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return Error("Group thread not found");
  }
  auto detail = GroupMembershipCodec::EncodeMemberRemoved(group_id, member_identity, metadata->roster_epoch);
  if (!detail) {
    return detail.error();
  }
  if (auto event = AppendMembershipSystemEvent((*thread)->id, GroupMembershipControlType::MemberRemoved,
                                               "Member removed", *detail, *local);
      !event) {
    return event.error();
  }
  return FanOutMembershipEvent(group_id, GroupMembershipControlType::MemberRemoved, WithSignedRoster(group_id, *detail),
                               "Member removed", *local, member_identity);
}

Roe<void> GroupMembershipWorkflow::RemoveMember(const std::string& group_id, const std::string& member_contact_id) {
  auto member_identity = ResolveRelayIdentity(member_contact_id);
  if (!member_identity) {
    return member_identity.error();
  }
  return RemoveMemberInternal(group_id, *member_identity);
}

Roe<void> GroupMembershipWorkflow::RemoveMemberByIdentity(const std::string& group_id,
                                                         const std::string& member_identity) {
  if (member_identity.empty()) {
    return Error("member_identity required");
  }
  return RemoveMemberInternal(group_id, member_identity);
}

Roe<void> GroupMembershipWorkflow::LeaveGroup(const std::string& group_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (*local == metadata->owner_identity) {
    return Error("Owner must transfer ownership before leaving");
  }

  const GroupMembershipCodec::MemberLeftPayload leave{group_id, *local, metadata->roster_epoch + 1};
  auto detail = GroupMembershipCodec::EncodeMemberLeft(group_id, *local, leave.roster_epoch);
  if (!detail) {
    return detail.error();
  }
  // Best-effort notify remaining members (while we still hold the roster); local leave always
  // proceeds so close cannot get stuck.
  (void)FanOutMembershipEvent(group_id, GroupMembershipControlType::MemberLeft, *detail, "Member left", *local);
  if (auto applied = ApplyMemberLeftToRoster(roster_, leave, *local); !applied) {
    log().warning << "LeaveGroup local roster: " << applied.error().message << " group=" << group_id;
  }
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

Roe<void> GroupMembershipWorkflow::LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto update = GroupRosterStore::LockForUpdate();  // the epoch read below and its apply, as one
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (*local != metadata->owner_identity) {
    return Error("Only the group owner may transfer ownership");
  }
  if (new_owner_identity.empty() || new_owner_identity == *local) {
    return Error("Choose a different member as the new owner");
  }
  auto is_member = roster_.IsMember(group_id, new_owner_identity);
  if (!is_member) {
    return is_member.error();
  }
  if (!*is_member) {
    return Error("Successor must be a group member");
  }
  if (IsMemberUnreachable(group_id, new_owner_identity)) {
    return Error("Successor is unreachable — dismiss locally or pick someone else");
  }

  const GroupMembershipCodec::OwnerTransferredPayload transfer{group_id, new_owner_identity,
                                                               metadata->roster_epoch + 1, /*leave_previous=*/true};
  if (auto applied = ApplyOwnerTransferredToRoster(roster_, transfer, *local); !applied) {
    return applied.error();
  }
  update.unlock();  // not across the sends below

  auto detail =
      GroupMembershipCodec::EncodeOwnerTransferred(group_id, new_owner_identity, transfer.roster_epoch, true);
  if (!detail) {
    return detail.error();
  }
  // G012: the hand-over, signed by us, lets members (and devices restored from an older locker snapshot) trust
  // the new owner's statements.
  std::string sent = *detail;
  if (auto identity = identity_.Get(); identity && !identity->account_signing_public_key_b64.empty()) {
    if (auto handed = group_roster_statement::SignTransfer(identity_, identity->account_signing_public_key_b64,
                                                           group_id, new_owner_identity, transfer.roster_epoch)) {
      if (auto with = group_roster_sync_codec::WithTransfer(sent, *handed)) {
        sent = std::move(*with);
      }
    } else {
      log().warning << "Owner transfer not signed group=" << group_id << ": " << handed.error().message;
    }
  }
  (void)FanOutMembershipEvent(group_id, GroupMembershipControlType::OwnerTransferred, sent,
                              "Group ownership transferred", *local);
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

bool GroupMembershipWorkflow::IsDissolved(const std::string& group_id) const {
  auto metadata = roster_.LoadMetadata(group_id);
  return metadata && *metadata && (*metadata)->sync_state == GroupSyncState::Dissolved;
}

void GroupMembershipWorkflow::AppendDissolvedLine(const std::string& group_id, const std::string& owner_identity,
                                                  const std::string& sender_identity) {
  auto added = EnsureDissolvedLine(store_, group_id, owner_identity, sender_identity);
  if (!added || !*added) {
    return;  // the chat was deleted, or already says so
  }
  if (auto thread = store_.GetThread(**added); thread && *thread) {
    Thread row = **thread;
    row.preview = Tr("chat.group.dissolved_preview");
    row.updated_at = util::NowUnixMs();
    (void)store_.UpsertThread(row);
  }
}

void GroupMembershipWorkflow::OnGroupDissolvedElsewhere(const GroupDissolvedEnd& end) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }
  // The owner's other device sees "You dissolved this group"; anyone else the owner's name.
  auto added = EnsureDissolvedLine(store_, end.group_id, end.owner_identity,
                                   end.owner_identity == *local ? std::string(kLocalSelfContactId) : end.owner_identity);
  if (added && *added) {
    mesh_messaging_.SurfaceGroupDissolved(**added);
  }
  for (const PendingGroupInvite& invite : end.voided_invites) {
    if (!invite.outbound) {
      (void)ResolveInviteCard(invite.inviter_identity, invite.invite_nonce, InviteStatus::Expired,
                              "This group was dissolved");
    }
  }
  log().info << "Group dissolved elsewhere group=" << end.group_id;
}

void GroupMembershipWorkflow::EndDissolvedFromLocker(const AccountStateGroup& group) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return;
  }
  auto member = roster_.IsMember(group.group_id, *local);
  if (!member || !*member || IsDissolved(group.group_id)) {
    return;  // not a group we are in, or already over
  }
  // Our own locker, written only by our own devices: no statement to check. It cannot be undone, so no epoch gate.
  auto ended = EndGroupInRoster(roster_, group.group_id, group.owner_identity, group.roster_epoch);
  if (ended && ended->applied) {
    OnGroupDissolvedElsewhere({group.group_id, group.owner_identity, std::move(ended->voided_invites)});
  }
}

Roe<void> GroupMembershipWorkflow::DissolveGroup(const std::string& group_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  std::string sibling_detail;
  {
    auto update = GroupRosterStore::LockForUpdate();  // the roster we sign and the one we end, as one
    auto metadata = LoadMetadataOrError(group_id);
    if (!metadata) {
      return metadata.error();
    }
    if (*local != metadata->owner_identity) {
      return Error("Only the group owner may dissolve the group");
    }
    if (metadata->sync_state == GroupSyncState::Dissolved) {
      update.unlock();
      ResumePendingGroupSends();  // asked again: finish what an earlier go left
      return {};
    }
    auto members = roster_.ListMembers(group_id);
    if (!members) {
      return members.error();
    }
    std::vector<std::string> recipients;
    for (const GroupRosterMember& m : *members) {
      if (m.member_identity != *local) {
        recipients.push_back(m.member_identity);
      }
    }
    // Signed before anything changes here: without a key nobody could be told, and the group would end for us alone.
    const uint64_t epoch = metadata->roster_epoch + 1;
    auto statement = SignDissolved(*metadata, *members, epoch);
    if (!statement && !recipients.empty()) {
      return statement.error();
    }
    std::string statement_json;
    std::string detail;
    if (statement) {
      statement_json = DumpJson(group_roster_statement::ToJson(*statement));
      detail = group_roster_sync_codec::EncodeStatement(*statement);
      sibling_detail = detail;
    }
    // What still has to be sent, written with the ending itself (one transaction): the dissolve to each member, and a
    // refusal to each invitee whose invite is still out, so none of them accepts into a group that is over.
    std::vector<PendingGroupSend> sends;
    for (const std::string& member : recipients) {
      sends.push_back({group_id, member, GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupDissolved),
                       detail, "The group owner dissolved this group"});
    }
    for (const PendingGroupInvite& invite : ListInvitedWaiting(group_id)) {
      auto refusal = GroupMembershipCodec::EncodeInviteRefused(invite.invite_nonce, group_id, kInviteRefusedGroupDissolved);
      if (refusal) {
        sends.push_back({group_id, invite.invitee_identity,
                         GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupInviteRefused), *refusal,
                         RefusedDisplay(kInviteRefusedGroupDissolved)});
      }
    }
    auto ended = EndGroupInRoster(roster_, group_id, *local, epoch, statement_json, sends);
    if (!ended) {
      return ended.error();
    }
    log().info << "Group dissolved group=" << group_id << " sends=" << sends.size();
  }
  AppendDissolvedLine(group_id, *local, kLocalSelfContactId);  // our own line reads "You dissolved…"
  // Our other devices end it too (M021 sibling lane; best effort — a device that was off finds the locker's tombstone).
  if (!sibling_detail.empty()) {
    if (auto notice = GroupMembershipCodec::BuildSystemMessage(std::string(), GroupMembershipControlType::GroupDissolvedSibling,
                                                               "Group dissolved", sibling_detail, *local)) {
      if (auto sent = mesh_messaging_.NotifyOwnDevices(*notice); !sent) {
        log().info << "Group dissolve not sent to our other devices: " << sent.error().message;
      }
    }
  }
  // Best-effort, through the outbox: each copy goes when it can; a member we cannot reach holds nobody back.
  ResumePendingGroupSends();
  return {};
}

void GroupMembershipWorkflow::ResumePendingGroupSends(const size_t attempt) {
  constexpr int64_t kRetryDelaysMs[] = {30'000, 120'000, 600'000};
  if (AppRuntime::IsShuttingDown()) {
    return;
  }
  auto pending = roster_.ListPendingSends();
  if (!pending) {
    log().warning << "Pending group sends not read: " << pending.error().message;
    return;
  }
  bool all_sent = true;
  for (const PendingGroupSend& send : *pending) {
    const auto type = GroupMembershipControlTypeFromWire(send.control_type);
    if (!type) {
      (void)roster_.DeletePendingSend(send);  // a type this app does not know cannot be sent
      continue;
    }
    // Done once the copy has reached the outbox (SendUserMessage keeps it and retries until it is delivered).
    if (auto sent = SendMembershipDirectMessage(send.recipient, *type, send.detail_json, send.display); sent) {
      (void)roster_.DeletePendingSend(send);
    } else {
      all_sent = false;
      log().warning << "Group send to " << send.recipient << " not queued (" << send.control_type
                    << "): " << sent.error().message;
    }
  }
  if (!all_sent && attempt < std::size(kRetryDelaysMs)) {
    AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kRetryDelaysMs[attempt]), [this, attempt]() {
      AppRuntime::PostWorkerNormal([this, attempt]() { ResumePendingGroupSends(attempt + 1); });
    });
  }
}

void GroupMembershipWorkflow::MarkMemberUnreachable(const std::string& group_id, const std::string& member_identity) {
  if (group_id.empty() || member_identity.empty()) {
    return;
  }
  {
    std::lock_guard lock(unreachable_mutex_);
    unreachable_.insert({group_id, member_identity});
  }
  auto metadata = roster_.LoadMetadata(group_id);
  if (metadata && *metadata && (**metadata).owner_identity == member_identity) {
    auto local = LocalRelayIdentity();
    if (local && *local != member_identity) {
      (void)EnsureOwnerUnreachableAdvisory(group_id);
    }
  }
}

void GroupMembershipWorkflow::ClearMemberUnreachable(const std::string& group_id,
                                                    const std::string& member_identity) {
  if (group_id.empty() || member_identity.empty()) {
    return;
  }
  bool was_owner = false;
  {
    std::lock_guard lock(unreachable_mutex_);
    unreachable_.erase({group_id, member_identity});
  }
  auto metadata = roster_.LoadMetadata(group_id);
  if (metadata && *metadata && (**metadata).owner_identity == member_identity) {
    was_owner = true;
  }
  if (was_owner) {
    (void)ResolveOwnerUnreachableAdvisory(group_id);
  }
}

bool GroupMembershipWorkflow::IsMemberUnreachable(const std::string& group_id,
                                                 const std::string& member_identity) const {
  std::lock_guard lock(unreachable_mutex_);
  return unreachable_.find({group_id, member_identity}) != unreachable_.end();
}

std::vector<std::string> GroupMembershipWorkflow::ListUnreachable(const std::string& group_id) const {
  std::vector<std::string> out;
  std::lock_guard lock(unreachable_mutex_);
  for (const auto& entry : unreachable_) {
    if (entry.first == group_id) {
      out.push_back(entry.second);
    }
  }
  return out;
}

std::vector<PendingGroupInvite> GroupMembershipWorkflow::ListInvitedWaiting(const std::string& group_id) const {
  auto invites = roster_.ListOutboundPendingInvites(group_id, util::NowUnixMs());
  return invites ? std::move(*invites) : std::vector<PendingGroupInvite>{};
}

bool GroupMembershipWorkflow::IsOwnerUnreachable(const std::string& group_id) const {
  auto metadata = roster_.LoadMetadata(group_id);
  if (!metadata || !*metadata || (*metadata)->sync_state == GroupSyncState::Dissolved) {
    return false;  // an ended group waits for no owner
  }
  return IsMemberUnreachable(group_id, (**metadata).owner_identity);
}

Roe<void> GroupMembershipWorkflow::EnsureOwnerUnreachableAdvisory(const std::string& group_id) {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return {};  // G016: an ended group has no owner to wait for
  }
  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return {};
  }
  auto messages = store_.GetMessagesPage((*thread)->id, std::nullopt, 500);
  if (!messages) {
    return messages.error();
  }
  for (const ThreadMessage& message : *messages) {
    if (GroupMembershipCodec::IsOwnerUnreachableAdvisory(message) &&
        !GroupMembershipCodec::IsOwnerUnreachableResolved(message)) {
      return {};
    }
  }

  const std::string title = "Owner hasn’t been reachable";
  const std::string body =
      "You can keep chatting with people who are online. Inviting, renaming for everyone, and removing "
      "members need the owner. If they’ve left for good, start a new group from this one.";
  Object detail_obj;
  detail_obj.set("group_id", group_id);
  detail_obj.set("owner_identity", metadata->owner_identity);
  const std::string detail = DumpJson(detail_obj);
  auto message = GroupMembershipCodec::BuildSystemMessage((*thread)->id,
                                                          GroupMembershipControlType::GroupOwnerUnreachable,
                                                          title + "\n" + body, detail, metadata->owner_identity);
  if (!message) {
    return message.error();
  }
  message->chat_actions =
      GroupMembershipCodec::BuildOwnerUnreachableChatActions(group_id, metadata->owner_identity);
  message->text = title;
  if (auto appended = store_.AppendMessage(*message); !appended) {
    return appended.error();
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::ResolveOwnerUnreachableAdvisory(const std::string& group_id) {
  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return {};
  }
  auto messages = store_.GetMessagesPage((*thread)->id, std::nullopt, 500);
  if (!messages) {
    return messages.error();
  }
  for (ThreadMessage& message : *messages) {
    if (!GroupMembershipCodec::IsOwnerUnreachableAdvisory(message) ||
        GroupMembershipCodec::IsOwnerUnreachableResolved(message)) {
      continue;
    }
    GroupMembershipCodec::ApplyOwnerUnreachableResolution(message);
    message.text = "Noted — you can keep chatting with people who are online.";
    (void)store_.UpdateMessage(message);
  }
  return {};
}

Roe<Thread> GroupMembershipWorkflow::OpenOwnerDirectMessage(const std::string& owner_identity) {
  if (owner_identity.empty()) {
    return Error("owner_identity required");
  }
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = owner_identity;
  direct_target.channel = ThreadChannel::E2ePublic;
  std::string contact_id;
  std::string title = owner_identity;
  if (auto contact = contacts_.FindByIdentity(owner_identity, mesh::ContactIdKind::Account)) {
    if (*contact) {
      contact_id = (*contact)->id;
      title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
      if (title.empty()) {
        title = owner_identity;
      }
    }
  }
  return store_.FindOrCreateDirectThread(direct_target, contact_id, title);
}

Roe<Thread> GroupMembershipWorkflow::ForkGroup(const std::string& group_id, const std::string& new_title,
                                              const std::vector<std::string>& member_contact_ids) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto source_thread = store_.FindGroupThread(group_id);
  if (!source_thread || !*source_thread) {
    return Error("Source group not found");
  }

  const std::string new_group_id = GenerateGroupId();
  GroupForkPayload fork;
  fork.source_group_id = group_id;
  fork.new_group_id = new_group_id;
  fork.new_group_title = new_title;
  fork.history_mode = GroupHistoryMode::Fresh;
  fork.actor_identity = *local;
  fork.roster_epoch = 1;
  for (const std::string& contact_id : member_contact_ids) {
    if (auto identity = ResolveRelayIdentity(contact_id)) {
      fork.selected_identities.push_back(*identity);
    }
  }

  auto new_thread = store_.FindOrCreateGroupThread(new_group_id, new_title, member_contact_ids);
  if (!new_thread) {
    return new_thread.error();
  }

  auto local_identity = LocalRelayIdentity();
  if (!local_identity) {
    return local_identity.error();
  }

  GroupMetadata metadata;
  metadata.group_id = new_group_id;
  metadata.owner_identity = *local_identity;
  metadata.title = new_title;
  metadata.roster_epoch = 1;
  if (auto saved = roster_.UpsertMetadata(metadata); !saved) {
    return saved.error();
  }

  GroupRosterMember owner;
  owner.member_identity = *local_identity;
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  (void)roster_.UpsertMember(new_group_id, owner);
  (void)roster_.UpsertGroupTarget(new_group_id, new_thread->id, 1, 1);

  for (const std::string& contact_id : member_contact_ids) {
    (void)InviteMember(new_group_id, contact_id);
  }

  auto fork_detail = GroupMembershipCodec::EncodeGroupForked(fork);
  if (!fork_detail) {
    return fork_detail.error();
  }
  (void)AppendMembershipSystemEvent((**source_thread).id, GroupMembershipControlType::GroupForked,
                                    "Group forked", *fork_detail, *local);
  (void)AppendMembershipSystemEvent(new_thread->id, GroupMembershipControlType::GroupForked, "Forked from group",
                                    *fork_detail, *local);
  return *new_thread;
}

Roe<void> GroupMembershipWorkflow::SendRenameDirectMessage(const std::string& member_identity,
                                                          const std::string& group_id, const std::string& title,
                                                          const uint64_t roster_epoch,
                                                          const uint64_t title_version) {
  auto detail = GroupMembershipCodec::EncodeGroupRenamed(group_id, title, roster_epoch, title_version);
  if (!detail) {
    return detail.error();
  }
  return SendMembershipDirectMessage(member_identity, GroupMembershipControlType::GroupRenamed, *detail,
                                     "Group renamed to " + title);
}

Roe<void> GroupMembershipWorkflow::RenameGroupShared(const std::string& group_id, const std::string& title) {
  if (title.empty()) {
    return Error("Group title required");
  }
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (*local != metadata->owner_identity) {
    return Error("Only the group owner may rename the group for everyone");
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);
  }
  if (!RoleHasPermission(MemberRole::Owner, kPermRename)) {
    return Error("Actor lacks rename permission");
  }

  metadata->title = title;
  metadata->title_version += 1;
  if (auto saved = roster_.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }

  auto thread = store_.FindGroupThread(group_id);
  if (!thread) {
    return thread.error();
  }
  if (*thread) {
    Thread updated = **thread;
    updated.title = title;
    updated.updated_at = util::NowUnixMs();
    if (auto upserted = store_.UpsertThread(updated); !upserted) {
      return upserted.error();
    }
    auto detail = GroupMembershipCodec::EncodeGroupRenamed(group_id, title, metadata->roster_epoch,
                                                           metadata->title_version);
    if (!detail) {
      return detail.error();
    }
    if (auto event = AppendMembershipSystemEvent(updated.id, GroupMembershipControlType::GroupRenamed,
                                                 "Group renamed to " + title, *detail, *local);
        !event) {
      return event.error();
    }
  }

  auto members = roster_.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == *local) {
      continue;
    }
    if (auto sent = SendRenameDirectMessage(member.member_identity, group_id, title, metadata->roster_epoch,
                                            metadata->title_version);
        !sent) {
      return sent.error();
    }
  }
  return {};
}

Roe<void> GroupMembershipWorkflow::ApplyInboundGroupRenamed(const GroupMembershipCodec::GroupRenamedPayload& payload,
                                                           const std::string& actor_identity) {
  auto applied = ApplyGroupRenamedToRoster(roster_, payload, actor_identity);
  if (!applied) {
    return applied.error();
  }
  if (!*applied) {
    return {};
  }

  auto thread = store_.FindGroupThread(payload.group_id);
  if (!thread) {
    return thread.error();
  }
  if (*thread) {
    Thread row = **thread;
    row.title = payload.title;
    row.updated_at = util::NowUnixMs();
    if (auto upserted = store_.UpsertThread(row); !upserted) {
      return upserted.error();
    }
    auto detail = GroupMembershipCodec::EncodeGroupRenamed(payload.group_id, payload.title, payload.roster_epoch,
                                                           payload.title_version);
    if (detail) {
      (void)AppendMembershipSystemEvent(row.id, GroupMembershipControlType::GroupRenamed,
                                        "Group renamed to " + payload.title, *detail, actor_identity);
    }
  }
  return {};
}

Roe<std::vector<GroupRosterMember>> GroupMembershipWorkflow::ListRoster(const std::string& group_id) const {
  return roster_.ListMembers(group_id);
}

Roe<bool> GroupMembershipWorkflow::IsLocalOwner(const std::string& group_id) const {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto owner = OwnerIdentity(group_id);
  if (!owner) {
    return owner.error();
  }
  return *local == *owner;
}

Roe<std::string> GroupMembershipWorkflow::OwnerIdentity(const std::string& group_id) const {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  return metadata->owner_identity;
}

Roe<void> GroupMembershipWorkflow::PublishMemberJoined(const std::string& group_id,
                                                      const std::string& member_identity,
                                                      const uint64_t roster_epoch) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto roster = roster_.ListMembers(group_id);
  if (!roster) {
    return roster.error();
  }
  GroupMembershipCodec::MemberJoinedPayload payload;
  payload.group_id = group_id;
  payload.member_identity = member_identity;
  payload.role = MemberRole::Member;
  payload.roster_epoch = roster_epoch;
  payload.members.reserve(roster->size());
  for (const GroupRosterMember& row : *roster) {
    payload.members.push_back({.member_identity = row.member_identity, .role = row.role});
  }
  auto plain = GroupMembershipCodec::EncodeMemberJoined(payload);
  if (!plain) {
    return plain.error();
  }
  auto thread = store_.FindGroupThread(group_id);
  if (thread && *thread) {
    (void)AppendMembershipSystemEvent((*thread)->id, GroupMembershipControlType::MemberJoined,
                                      "Member joined the group", *plain, *local);
  }

  // Members key each other pairwise: the members get the newcomer's card, the newcomer everyone's
  // (docs/contracts/ACCOUNT_CARD.md). The snapshot backfills prior co-invitees for late joiners.
  auto kept_card = [this](const std::string& account_id) -> std::optional<Object> {
    auto kept = account_keys_.Kept(account_id);
    if (!kept || !*kept) {
      return std::nullopt;
    }
    return CarriedCardJson(**kept);
  };
  GroupMembershipCodec::MemberJoinedPayload for_members = payload;
  GroupMembershipCodec::MemberJoinedPayload for_newcomer = payload;
  size_t card_bytes = 0;
  for (size_t i = 0; i < payload.members.size(); ++i) {
    const std::string& identity = payload.members[i].member_identity;
    if (identity == *local) {
      continue;  // the newcomer has the owner's keys and route from the invite's chat
    }
    if (auto route = mesh_messaging_.RelayRouteOf(identity)) {
      for_members.members[i].relay_user_id = *route;
      for_newcomer.members[i].relay_user_id = *route;
    }
    auto card = kept_card(identity);
    if (!card) {
      continue;
    }
    if (identity == member_identity) {
      for_members.members[i].card = std::move(card);
      continue;
    }
    card_bytes += DumpJson(*card).size();
    if (card_bytes <= kRosterCardsBudgetBytes) {
      for_newcomer.members[i].card = std::move(card);
    }
  }
  auto members_encoded = GroupMembershipCodec::EncodeMemberJoined(for_members);
  auto newcomer_encoded = GroupMembershipCodec::EncodeMemberJoined(for_newcomer);
  if (!members_encoded || !newcomer_encoded) {
    return Error("member_joined: could not encode");
  }
  // Both carry the owner's statement of the roster with the newcomer in it (G012), signed once.
  std::string members_detail = *members_encoded;
  std::string newcomer_detail = *newcomer_encoded;
  if (auto statement = SignRoster(group_id); statement) {
    if (auto with = group_roster_sync_codec::WithStatement(members_detail, *statement)) {
      members_detail = std::move(*with);
    }
    if (auto with = group_roster_sync_codec::WithStatement(newcomer_detail, *statement)) {
      newcomer_detail = std::move(*with);
    }
  } else {
    log().warning << "Roster not signed group=" << group_id << ": " << statement.error().message;
  }
  // Best-effort, like every membership event.
  std::vector<std::string> identities;
  identities.reserve(roster->size());
  for (const GroupRosterMember& row : *roster) {
    if (row.member_identity != member_identity) {
      identities.push_back(row.member_identity);
    }
  }
  (void)FanOutPairwise(SelectPairwiseFanoutTargets(identities, *local), PairwiseFanoutMode::BestEffort,
                       [&](const std::string& identity) {
                         return SendMembershipDirectMessage(identity, GroupMembershipControlType::MemberJoined,
                                                            members_detail, "Member joined the group", *plain);
                       });
  // Our copies keep the roster, not the cards (they are kept as cards on the other side).
  (void)SendMembershipDirectMessage(member_identity, GroupMembershipControlType::MemberJoined, newcomer_detail,
                                    "Member joined the group", *plain);
  return {};
}

std::string GroupMembershipWorkflow::WithSignedRoster(const std::string& group_id, const std::string& detail_json) {
  auto statement = SignRoster(group_id);
  if (!statement) {
    log().warning << "Roster not signed group=" << group_id << ": " << statement.error().message;
    return detail_json;
  }
  auto with = group_roster_sync_codec::WithStatement(detail_json, *statement);
  return with ? *with : detail_json;
}

GroupMembershipWorkflow::RosterParts GroupMembershipWorkflow::RosterPartsOf(
    const GroupMetadata& metadata, const std::vector<GroupRosterMember>& members,
    const std::optional<GroupOwnerTransfer>& handed) const {
  RosterParts parts;
  parts.rows.reserve(members.size());
  for (const GroupRosterMember& m : members) {
    parts.rows.push_back({m.member_identity, m.role, m.joined_at});
  }
  if (auto held = group_roster_sync::Held(metadata)) {
    parts.chain = held->owner_chain;
  }
  if (handed) {
    parts.chain.push_back(*handed);
  }
  return parts;
}

Roe<std::string> GroupMembershipWorkflow::SigningKey() const {
  auto identity = identity_.Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->account_signing_public_key_b64.empty()) {
    return Error("No account signing key yet");
  }
  return identity->account_signing_public_key_b64;
}

Roe<GroupRosterStatement> GroupMembershipWorkflow::SignDissolved(const GroupMetadata& metadata,
                                                                 const std::vector<GroupRosterMember>& members,
                                                                 const uint64_t epoch) {
  auto key = SigningKey();
  if (!key) {
    return key.error();
  }
  RosterParts parts = RosterPartsOf(metadata, members);
  return group_roster_statement::Sign(identity_, *key, metadata.group_id, epoch, std::move(parts.rows),
                                      std::move(parts.chain), /*dissolved=*/true);
}

Roe<GroupRosterStatement> GroupMembershipWorkflow::SignRoster(const std::string& group_id,
                                                              const std::optional<GroupOwnerTransfer>& handed) {
  auto key = SigningKey();
  if (!key) {
    return key.error();
  }
  auto account = identity_.GetAccountId();
  if (!account) {
    return account.error();
  }
  auto update = GroupRosterStore::LockForUpdate();  // the roster read and the statement kept, as one
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity != *account) {
    return Error("Only the group owner signs its roster");
  }
  auto members = roster_.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  RosterParts parts = RosterPartsOf(*metadata, *members, handed);
  auto statement = group_roster_statement::Sign(identity_, *key, group_id, metadata->roster_epoch,
                                                std::move(parts.rows), std::move(parts.chain));
  if (!statement) {
    return statement.error();
  }
  if (auto kept = group_roster_sync::KeepStatement(roster_, *statement); !kept) {
    return kept.error();
  }
  return statement;
}

Roe<void> GroupMembershipWorkflow::SignAndSendRoster(const std::string& group_id,
                                                     const std::optional<GroupOwnerTransfer>& handed) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto statement = SignRoster(group_id, handed);
  if (!statement) {
    return statement.error();
  }
  std::vector<std::string> members;
  for (const GroupRosterStatementMember& m : statement->members) {
    members.push_back(m.identity);
  }
  const std::string detail = group_roster_sync_codec::EncodeStatement(*statement);
  (void)FanOutPairwise(SelectPairwiseFanoutTargets(members, *local), PairwiseFanoutMode::BestEffort,
                       [&](const std::string& identity) {
                         return SendMembershipDirectMessage(identity, GroupMembershipControlType::GroupRosterStatement,
                                                            detail, "Group members updated");
                       });
  return {};
}

Roe<void> GroupMembershipWorkflow::AnswerRosterRequest(const group_roster_sync_codec::RosterRequest& request,
                                                       const std::string& asker) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto metadata = roster_.LoadMetadata(request.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return {};
  }
  group_roster_sync_codec::RosterReply reply;
  if ((*metadata)->sync_state == GroupSyncState::Dissolved && (*metadata)->owner_identity == *local) {
    // G016: the owner answers with its dissolve, signed again over the roster it held (else the one it kept).
    reply.group_id = request.group_id;
    reply.request_id = request.request_id;
    const auto held = group_roster_sync::Held(**metadata);
    std::vector<GroupRosterMember> stated;
    for (const GroupRosterStatementMember& m : held ? held->members : std::vector<GroupRosterStatementMember>{}) {
      stated.push_back({m.identity, std::string(), m.role, m.joined_at});
    }
    if (auto fresh = SignDissolved(**metadata, stated, (*metadata)->roster_epoch)) {
      reply.statement = std::move(*fresh);
    } else {
      reply.statement = held;
    }
  } else if ((*metadata)->owner_identity == *local && (*metadata)->sync_state == GroupSyncState::Active) {
    // The owner answers with the roster as it is now.
    auto statement = SignRoster(request.group_id);
    if (!statement) {
      return statement.error();
    }
    reply.group_id = request.group_id;
    reply.request_id = request.request_id;
    reply.removed = !statement->Lists(asker);
    reply.statement = std::move(*statement);
  } else {
    auto answer = group_roster_sync::Answer(roster_, request, asker);
    if (!answer) {
      return answer.error();
    }
    if (!*answer) {
      return {};
    }
    reply = std::move(**answer);
  }
  log().info << "Roster answer group=" << request.group_id << " to=" << asker
             << " statement=" << (reply.statement ? reply.statement->roster_epoch : 0) << " removed=" << reply.removed;
  return SendRosterControl(asker, GroupMembershipControlType::GroupRosterReply,
                           group_roster_sync_codec::EncodeReply(reply), "Group members", *local);
}

Roe<void> GroupMembershipWorkflow::StartRosterCheck(const std::string& group_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto check = group_roster_sync::BeginCheck(roster_, group_id, *local, util::NowUnixMs());
  if (!check) {
    return check.error();
  }
  log().info << "Roster check group=" << group_id << " request_id=" << check->request.request_id
             << " asking=" << check->ask.size();
  const std::string detail = group_roster_sync_codec::EncodeRequest(check->request);
  (void)FanOutPairwise(check->ask, PairwiseFanoutMode::BestEffort, [&](const std::string& identity) {
    return SendRosterControl(identity, GroupMembershipControlType::GroupRosterRequest, detail,
                             "Checking group members", *local);
  });
  return {};
}

Roe<void> GroupMembershipWorkflow::SendRosterControl(const std::string& peer_identity,
                                                     const GroupMembershipControlType control_type,
                                                     const std::string& detail_json, const std::string& display,
                                                     const std::string& local_identity) {
  // On the control lane (MakeGroupRosterSyncFamily): no chat row on either side, and an account copy with
  // our card — the asker may be a device restored from the account locker, which keeps none of the cards it had.
  auto control = GroupMembershipCodec::BuildSystemMessage(std::string(), control_type, display, detail_json,
                                                          local_identity);
  if (!control) {
    return control.error();
  }
  return mesh_messaging_.SendControl(peer_identity, *control);
}

Roe<std::vector<AccountStateGroup>> GroupMembershipWorkflow::StateGroups() const {
  auto ids = roster_.ListGroupIds();
  if (!ids) {
    return ids.error();
  }
  const auto local = LocalRelayIdentity();
  std::vector<AccountStateGroup> groups;
  for (const std::string& group_id : *ids) {
    auto metadata = roster_.LoadMetadata(group_id);
    auto members = roster_.ListMembers(group_id);
    if (!metadata || !*metadata || !members) {
      continue;
    }
    AccountStateGroup group;
    group.group_id = group_id;
    group.owner_identity = (*metadata)->owner_identity;
    group.roster_epoch = (*metadata)->roster_epoch;
    const bool member = local && std::any_of(members->begin(), members->end(), [&](const GroupRosterMember& m) {
                          return m.member_identity == *local;
                        });
    if ((*metadata)->sync_state == GroupSyncState::Dissolved) {
      group.left = true;
      group.dissolved = true;  // terminal: no copy of the group, whatever its epoch, brings it back (G016)
      groups.push_back(std::move(group));
      continue;
    }
    if ((*metadata)->sync_state == GroupSyncState::Removed || (local && !member)) {
      group.left = true;  // a tombstone: an older copy where we were in must not bring it back
      groups.push_back(std::move(group));
      continue;
    }
    group.title = (*metadata)->title;
    group.title_version = (*metadata)->title_version;
    group.policy_json = GroupMembershipCodec::EncodeGroupPolicy((*metadata)->policy);
    group.roster_statement_json = (*metadata)->roster_statement_json;
    for (const GroupRosterMember& m : *members) {
      group.members.push_back({m.member_identity, MemberRoleToString(m.role), m.joined_at, m.contact_id});
    }
    groups.push_back(std::move(group));
  }
  return groups;
}

Roe<void> GroupMembershipWorkflow::RestoreGroup(const AccountStateGroup& group) {
  auto update = GroupRosterStore::LockForUpdate();
  GroupMetadata metadata;
  metadata.group_id = group.group_id;
  metadata.owner_identity = group.owner_identity;
  metadata.title = group.title;
  metadata.roster_epoch = group.roster_epoch;
  metadata.title_version = group.title_version;
  if (auto policy = GroupMembershipCodec::DecodeGroupPolicy(group.policy_json)) {
    metadata.policy = *policy;
  }
  metadata.roster_statement_json = group.roster_statement_json;
  metadata.sync_state = GroupSyncState::Checking;
  if (auto saved = roster_.UpsertMetadata(metadata); !saved) {
    return saved.error();
  }
  for (const AccountStateGroupMember& m : group.members) {
    GroupRosterMember member;
    member.member_identity = m.identity;
    member.role = MemberRoleFromString(m.role);
    member.joined_at = m.joined_at;
    member.contact_id = m.contact_id;
    if (auto upserted = roster_.UpsertMember(group.group_id, member); !upserted) {
      return upserted.error();
    }
  }
  update.unlock();
  if (auto thread = store_.FindOrCreateGroupThread(group.group_id, group.title, {}); !thread) {
    return thread.error();
  }
  log().info << "Group restored group=" << group.group_id << " members=" << group.members.size()
             << " (checking its members)";
  return {};
}

} // namespace pbr
