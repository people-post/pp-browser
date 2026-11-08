#include "feature/conversations/GroupMembershipWorkflow.h"

#include "lib/messaging/GroupPermissions.h"
#include "lib/messaging/GroupRosterVersion.h"

#include "lib/delivery/DirectChatTargetFromContact.h"
#include "lib/messaging/GroupChangeLines.h"
#include "lib/messaging/GroupLinkJoin.h"
#include "lib/messaging/GroupMembershipApply.h"
#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/delivery/PairwiseFanoutLogic.h"
#include "lib/delivery/SendRelayOptions.h"
#include "lib/people/ContactJson.h"

#include "common/Localized.h"
#include "common/runtime/AppRuntime.h"

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

/** A group whose invite named no title. */
constexpr const char kDefaultGroupTitle[] = "Group chat";

/** The line an app that does not know `group_invite_refused` shows for it. */
std::string RefusedDisplay(const std::string& reason) {
  return reason == kInviteRefusedGroupDissolved ? "Group invitation refused: the group was dissolved"
                                                : "Group invitation refused: the group is full";
}

} // namespace

GroupMembershipWorkflow::GroupMembershipWorkflow(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                               IGroupRosterStore& roster, GroupInviteGate& invite_gate,
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
  GroupRosterVersion::Bump();  // every local membership change writes one of these lines
  return {};
}

Roe<void> GroupMembershipWorkflow::SendInviteDirectMessage(const GroupInvitePayload& invite,
                                                          const std::string& invitee_contact_id) {
  // The DM with the invitee: their contact's, else (G022: someone met in a call) the one with their account.
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  direct_target.peer_identity_value = invite.invitee_identity;
  direct_target.channel = ThreadChannel::E2ePublic;
  std::string contact_id = invitee_contact_id;
  std::string dm_title = invite.invitee_identity;
  if (!contact_id.empty()) {
    auto contact = contacts_.Get(contact_id);
    if (!contact || !*contact) {
      return Error("Invitee contact not found");
    }
    direct_target = DirectChatTargetFromContact(**contact, ThreadChannel::E2ePublic);
    dm_title = (*contact)->display_name;
  } else if (auto contact = contacts_.FindByIdentity(invite.invitee_identity, mesh::ContactIdKind::Account);
             contact && *contact) {
    contact_id = (*contact)->id;
    dm_title = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
  }
  auto thread = store_.FindOrCreateDirectThread(direct_target, contact_id, dm_title);
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

Roe<Thread> GroupMembershipWorkflow::StartGroup(const std::string& title, const std::vector<std::string>& participants,
                                                const size_t invitee_count, const std::string& first_line_context) {
  auto local_identity = LocalRelayIdentity();
  if (!local_identity) {
    return local_identity.error();
  }

  if (invitee_count + 1 > kMaxGroupMembers) {
    return Error(kGroupFullError);  // G014: the owner and its members
  }
  const std::string group_id = GenerateGroupId();

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

  GroupMembershipCodec::MemberJoinedPayload joined;
  joined.group_id = group_id;
  joined.member_identity = *local_identity;
  joined.role = MemberRole::Owner;
  joined.roster_epoch = 1;
  joined.context = first_line_context;
  auto joined_detail = GroupMembershipCodec::EncodeMemberJoined(joined);
  if (!joined_detail) {
    return joined_detail.error();
  }
  const char* line = first_line_context.empty() ? "Owner created the group" : kGroupMadeInCallLine;
  if (auto event = AppendMembershipSystemEvent(thread->id, GroupMembershipControlType::MemberJoined, line,
                                               *joined_detail, *local_identity);
      !event) {
    return event.error();
  }
  return *thread;
}

Roe<Thread> GroupMembershipWorkflow::CreateGroup(const std::string& title,
                                                 const std::vector<std::string>& member_contact_ids) {
  auto thread = StartGroup(title, member_contact_ids, member_contact_ids.size(), {});
  if (!thread) {
    return thread.error();
  }
  for (const std::string& contact_id : member_contact_ids) {
    if (auto invited = InviteMember(thread->group_id.value_or(""), contact_id); !invited) {
      return invited.error();
    }
  }
  return *thread;
}

Roe<Thread> GroupMembershipWorkflow::CreateGroupFromCall(const std::string& title, const std::string& call_id,
                                                         const std::vector<std::string>& member_identities) {
  if (call_id.empty()) {
    return Error("call_id required");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  std::vector<std::string> invitees;
  for (const std::string& identity : member_identities) {
    if (!identity.empty() && identity != *local &&
        std::find(invitees.begin(), invitees.end(), identity) == invitees.end()) {
      invitees.push_back(identity);
    }
  }
  if (invitees.empty()) {
    return Error("Pick someone from the call");
  }
  // The thread names contacts where they are (people met in the call may not be).
  std::vector<std::string> participants;
  for (const std::string& identity : invitees) {
    if (auto contact = contacts_.FindByIdentity(identity, mesh::ContactIdKind::Account); contact && *contact) {
      participants.push_back((*contact)->id);
    }
  }
  auto thread = StartGroup(title, participants, invitees.size(), kGroupInviteContextCall);
  if (!thread) {
    return thread.error();
  }
  // Each invite best-effort: one participant out of reach does not keep the others out.
  const std::string group_id = thread->group_id.value_or("");
  for (const std::string& identity : invitees) {
    if (auto invited = InviteIdentity(group_id, identity, {}, call_id); !invited) {
      log().warning << "call group invite not sent group=" << group_id << ": " << invited.error().message;
    }
  }
  return *thread;
}

Roe<void> GroupMembershipWorkflow::InviteMember(const std::string& group_id, const std::string& invitee_contact_id) {
  auto invitee_identity = ResolveRelayIdentity(invitee_contact_id);
  if (!invitee_identity) {
    return invitee_identity.error();
  }
  return InviteIdentity(group_id, *invitee_identity, invitee_contact_id, {});
}

Roe<void> GroupMembershipWorkflow::InviteFromCall(const std::string& group_id, const std::string& invitee_identity,
                                                  const std::string& call_id) {
  if (invitee_identity.empty() || call_id.empty()) {
    return Error("An invite made in a call names the person and the call");
  }
  return InviteIdentity(group_id, invitee_identity, {}, call_id);
}

Roe<void> GroupMembershipWorkflow::InviteByIdentity(const std::string& group_id, const std::string& invitee_identity) {
  if (invitee_identity.empty()) {
    return Error("Choose someone to invite");
  }
  std::string contact_id;
  if (auto contact = contacts_.FindByIdentity(invitee_identity, mesh::ContactIdKind::Account); contact && *contact) {
    contact_id = (*contact)->id;
  }
  return InviteIdentity(group_id, invitee_identity, contact_id, {});
}

Roe<void> GroupMembershipWorkflow::InviteIdentity(const std::string& group_id, const std::string& invitee_identity,
                                                  const std::string& invitee_contact_id, const std::string& call_id) {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);  // G016: no invites into an ended group
  }
  // G019: whoever the preset lets invite (G017: Open, any member; Managed, the owner and admins).
  const std::optional<MemberRole> role = LocalRole(group_id);
  if (!role || !GroupAllows(GroupAction::Invite, *role, metadata->policy.preset)) {
    return Error(kGroupInviteNotAllowedError);
  }
  if (*role != MemberRole::Owner && !group_roster_sync::MaySend(*metadata)) {
    return Error(metadata->sync_state == GroupSyncState::Removed ? group_roster_sync::kGroupRemovedError
                                                                 : group_roster_sync::kGroupCheckingError);
  }
  // G020 5: someone removed comes back only through the owner or an admin (their add then re-admits, R2).
  if (!GroupAllows(GroupAction::Readmit, *role, metadata->policy.preset) &&
      group_roster_sync::RemovedEntry(roster_, group_id, invitee_identity, ChangeCards(), util::NowUnixMs())) {
    return Error(kGroupInviteRemovedError);
  }
  // G014: the active roster only — pending invites do not count (an accept that comes when full is refused).
  auto members = roster_.ListMembers(group_id);
  if (members && members->size() >= kMaxGroupMembers) {
    return Error(kGroupFullError);
  }

  GroupInvitePayload invite;
  invite.group_id = group_id;
  invite.group_title = metadata->title;
  invite.inviter_identity = *local;
  invite.invitee_identity = invitee_identity;
  invite.invite_nonce = util::GenerateUuid();
  invite.roster_epoch = metadata->roster_epoch;
  invite.expires_at = util::NowUnixMs() + kGroupInviteTtlMs;
  invite.actor_role = *role;
  invite.owner_identity = metadata->owner_identity;
  invite.member_count = members ? members->size() : 0; // G019 3: the count, never the members
  if (!call_id.empty()) {
    invite.context = kGroupInviteContextCall; // G022: the invitee's device shows it in that call
    invite.call_id = call_id;
  }

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
  // Pending only — the invitee is not an encrypt target until the owner's member_joined or our signed add (G019).
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

Roe<Thread> GroupMembershipWorkflow::EnterAcceptedGroup(const PendingGroupInvite& pending, const std::string& local,
                                                        const bool by_link) {
  const std::string title = pending.group_title.empty() ? std::string(kDefaultGroupTitle) : pending.group_title;
  const uint64_t roster_epoch = pending.roster_epoch == 0 ? 1 : pending.roster_epoch;

  // G019: the owner the invite named (a member's invite), else the inviter; the join bundle's statement must verify
  // for it.
  const std::string owner_identity = pending.Owner();
  GroupMetadata metadata;
  metadata.group_id = pending.group_id;
  metadata.owner_identity = owner_identity;
  metadata.title = title;
  metadata.roster_epoch = roster_epoch;
  if (auto saved = roster_.UpsertMetadata(metadata); !saved) {
    return saved.error();
  }

  auto thread = store_.FindOrCreateGroupThread(pending.group_id, title, {});
  if (!thread) {
    return thread.error();
  }

  // Seed both sides of the v1 create-invite pair so SendGroupMessage can fan out.
  // (Owner already has self; invitee must also know the owner.)
  // Until the join bundle comes: the owner and the inviter (it rewrites the roster from what it proves).
  GroupRosterMember owner;
  owner.member_identity = owner_identity;
  owner.role = MemberRole::Owner;
  owner.joined_at = util::NowUnixMs();
  if (auto upsert_owner = roster_.UpsertMember(pending.group_id, owner); !upsert_owner) {
    return upsert_owner.error();
  }
  if (pending.inviter_identity != owner_identity) {
    GroupRosterMember inviter;
    inviter.member_identity = pending.inviter_identity;
    inviter.joined_at = owner.joined_at;
    (void)roster_.UpsertMember(pending.group_id, inviter);
  }

  GroupRosterMember member;
  member.member_identity = local;
  member.role = MemberRole::Member;
  member.joined_at = util::NowUnixMs();
  if (auto upsert = roster_.UpsertMember(pending.group_id, member); !upsert) {
    return upsert.error();
  }
  (void)roster_.UpdateInviteStatus(pending.invite_nonce, InviteStatus::Accepted);
  (void)roster_.UpsertGroupTarget(pending.group_id, thread->id, 1, 1);

  // G022: a group made during a call this device was in says so on its first line.
  const bool made_in_call = !invite_gate_.CallOfInvite(pending.invite_nonce).empty();
  invite_gate_.ForgetInviteInCall(pending.invite_nonce);
  GroupMembershipCodec::MemberJoinedPayload joined;
  joined.group_id = pending.group_id;
  joined.member_identity = local;
  joined.role = MemberRole::Member;
  joined.roster_epoch = roster_epoch;
  joined.context = made_in_call ? kGroupInviteContextCall : "";
  if (by_link) { // G021: "You joined by Alice's link"
    joined.added_by = pending.inviter_identity;
    joined.by_link = true;
  }
  auto detail = GroupMembershipCodec::EncodeMemberJoined(joined);
  if (!detail) {
    return detail.error();
  }
  if (auto event =
          AppendMembershipSystemEvent(thread->id, GroupMembershipControlType::MemberJoined,
                                      made_in_call ? kGroupMadeInCallLine : "You joined the group", *detail, local);
      !event) {
    return event.error();
  }
  return *thread;
}

Roe<Thread> GroupMembershipWorkflow::JoinByLink(const std::string& request_id, const std::string& group_id,
                                                const std::string& title, const std::string& owner_identity,
                                                const std::string& answerer) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  // The join's own record, as an accepted invite from the answerer: the join bundle is taken from them for it alone.
  const PendingGroupInvite pending =
      group_link_join::JoinRecord(request_id, group_id, title, owner_identity, answerer, *local, util::NowUnixMs());
  if (auto kept = roster_.UpsertPendingInvite(pending); !kept) {
    return kept.error();
  }
  if (auto in = roster_.IsMember(group_id, *local); in && *in && !IsDissolved(group_id)) {
    if (auto thread = store_.FindGroupThread(group_id); thread && *thread) {
      return **thread; // in already: the answerer sends the bundle again
    }
  }
  return EnterAcceptedGroup(pending, *local, /*by_link=*/true);
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
    (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Expired); // G016: an invite to an ended group is void
    return Error(group_roster_sync::kGroupDissolvedError);
  }

  auto thread = EnterAcceptedGroup(**pending, *local);
  if (!thread) {
    return thread.error();
  }
  const std::string title = (*pending)->group_title.empty() ? kDefaultGroupTitle : (*pending)->group_title;
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
  invite_gate_.ForgetInviteInCall(invite_nonce);
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
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);
  }
  if (member_identity == *local) {
    return Error(*local == metadata->owner_identity ? "Owner cannot remove self — transfer ownership or leave"
                                                    : "Leave the group instead");
  }
  // G018: the removal is this device's signed `remove` (it names the adds and changes of the member it holds).
  auto device = OwnDevice();
  auto draft =
      device ? group_roster_sync::DraftRemove(roster_, group_id, *local, *device, member_identity, util::NowUnixMs())
             : Roe<GroupChange>(device.error());
  std::optional<GroupChange> removal;
  if (draft) {
    if (auto signed_change = SignOwnChange(std::move(*draft))) {
      removal = std::move(*signed_change);
    } else {
      log().warning << "remove not signed group=" << group_id << ": " << signed_change.error().message;
    }
  } else if (*local != metadata->owner_identity) {
    return draft.error(); // an admin's removal is its signed change, or nothing
  }
  if (*local == metadata->owner_identity) {
    return RemoveAsOwner(group_id, member_identity, removal);
  }
  if (!removal) {
    return Error("The removal could not be signed on this device");
  }
  return RemoveAsAdmin(group_id, *removal);
}

Roe<void> GroupMembershipWorkflow::RemoveAsOwner(const std::string& group_id, const std::string& member_identity,
                                                 const std::optional<GroupChange>& removal) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto update = IGroupRosterStore::LockForUpdate(); // the epoch read below and its apply, as one
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (removal) {
    // Folded by the statement signed below: its id is the removal a re-admit names (G018 R2).
    (void)group_roster_sync::HoldChange(roster_, *removal, ChangeCards(), util::NowUnixMs(), /*line_shown=*/true);
  }
  // The same change every member applies from the event (one rule for both replicas).
  const GroupMembershipCodec::MemberRemovedPayload event{group_id, member_identity, metadata->roster_epoch + 1};
  if (auto applied = ApplyMemberRemovedToRoster(roster_, event, *local); !applied) {
    return applied.error();
  }
  metadata->roster_epoch = event.roster_epoch;
  auto detail = GroupMembershipCodec::EncodeMemberRemoved(group_id, member_identity, metadata->roster_epoch, *local);
  if (!detail) {
    return detail.error();
  }
  // Signed under the same lock as the removal: a change arriving in between cannot be lost from the fold.
  const std::string sent = WithSignedRoster(group_id, *detail);
  update.unlock();  // not across the sends below
  // A removed member must not keep showing as "invited, waiting".
  (void)roster_.ExpireOutboundInvitesTo(group_id, member_identity);
  // The removed member reads nothing that follows: no copy is made for it (every copy is pairwise, G011).
  ClearMemberUnreachable(group_id, member_identity);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return Error("Group thread not found");
  }
  if (auto line = AppendMembershipSystemEvent((*thread)->id, GroupMembershipControlType::MemberRemoved,
                                              "Member removed", *detail, *local);
      !line) {
    return line.error();
  }
  // The roster has lost them already: the members first, then the one removed (G020 2).
  return FanOutMembershipEvent(group_id, GroupMembershipControlType::MemberRemoved, sent, "Member removed", *local,
                               member_identity);
}

Roe<void> GroupMembershipWorkflow::RemoveAsAdmin(const std::string& group_id, const GroupChange& removal) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const int64_t now_ms = util::NowUnixMs();
  auto held = group_roster_sync::HoldChange(roster_, removal, ChangeCards(), now_ms, /*line_shown=*/true);
  if (!held) {
    return held.error();
  }
  if (*held != group_roster_sync::ChangeHeld::Proved) {
    return Error("The removal does not prove itself on this device (its card is not here)");
  }
  FanOutChange(group_id, removal, removal.member); // while the roster still lists everyone to tell
  auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms);
  if (!applied) {
    return applied.error();
  }
  (void)roster_.ExpireOutboundInvitesTo(group_id, removal.member);
  ClearMemberUnreachable(group_id, removal.member);
  if (auto metadata = roster_.LoadMetadata(group_id); metadata && *metadata) {
    (void)group_change_lines::Append(store_, group_id, {removal}, (*metadata)->roster_epoch, *local);
  }
  log().info << "member removed by admin group=" << group_id << " member=" << removal.member;
  return {};
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
  // G018: our signed `leave` (it names the adds that brought us in and our own changes the owner has not folded),
  // to every member while we still hold the roster. Best-effort: leaving here always proceeds, so a close cannot get
  // stuck.
  const int64_t now_ms = util::NowUnixMs();
  auto device = OwnDevice();
  auto draft = device ? group_roster_sync::DraftLeave(roster_, group_id, *local, *device, now_ms)
                      : Roe<GroupChange>(device.error());
  auto leave = draft ? SignOwnChange(std::move(*draft)) : Roe<GroupChange>(draft.error());
  if (leave) {
    (void)group_roster_sync::HoldChange(roster_, *leave, ChangeCards(), now_ms, /*line_shown=*/true);
    FanOutChange(group_id, *leave);
    if (auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms); !applied) {
      log().warning << "LeaveGroup roster: " << applied.error().message << " group=" << group_id;
    }
  } else {
    log().warning << "LeaveGroup: no signed leave (" << leave.error().message << ") group=" << group_id;
  }
  if (auto still = roster_.IsMember(group_id, *local); still && *still) {
    (void)roster_.RemoveMember(group_id, *local); // the leave did not count here: we are out all the same
  }
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

Roe<std::string> GroupMembershipWorkflow::OwnDevice() {
  return mesh_messaging_.OwnAccountCards().DevicePeerId();
}

GroupCardLookup GroupMembershipWorkflow::ChangeCards() {
  return mesh_messaging_.GroupChangeCards();
}

Roe<GroupChange> GroupMembershipWorkflow::SignOwnChange(GroupChange draft) {
  // The first send of a change carries our card: a member who never had a pair session with us can check it.
  if (auto card = mesh_messaging_.OwnAccountCards().Card(util::NowUnixMs())) {
    draft.signer_card = std::move(*card);
  }
  return group_change::Sign(std::move(draft),
                            [this](const ByteVector& bytes) { return identity_.SignDeviceBytes(bytes); });
}

void GroupMembershipWorkflow::FanOutChange(const std::string& group_id, const GroupChange& change,
                                           const std::string& removed,
                                           const std::optional<group_roster_sync_codec::ChangeNewcomer>& newcomer) {
  auto local = LocalRelayIdentity();
  auto members = roster_.ListMembers(group_id);
  if (!local || !members) {
    return;
  }
  const std::string skip_newcomer = newcomer ? newcomer->member_identity : std::string();
  std::vector<std::string> identities;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity != removed && member.member_identity != skip_newcomer) {
      identities.push_back(member.member_identity);
    }
  }
  const std::string detail = group_roster_sync_codec::EncodeChange(change, newcomer);
  // Our copies keep the change, not the newcomer's card.
  const std::optional<std::string> stored =
      newcomer ? std::optional<std::string>(group_roster_sync_codec::EncodeChange(change)) : std::nullopt;
  const std::string display = "Group members changed";
  (void)FanOutPairwise(SelectPairwiseFanoutTargets(identities, *local), PairwiseFanoutMode::BestEffort,
                       [&](const std::string& identity) {
                         return SendMembershipDirectMessage(identity, GroupMembershipControlType::GroupChange, detail,
                                                            display, stored);
                       });
  if (!removed.empty() && removed != *local) {
    (void)SendMembershipDirectMessage(removed, GroupMembershipControlType::GroupChange, detail, display);
  }
}

Roe<void> GroupMembershipWorkflow::LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto update = IGroupRosterStore::LockForUpdate();  // the epoch read below and its apply, as one
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
    row.preview = Localized("chat.group.dissolved_preview");
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

GroupMembershipWorkflow::~GroupMembershipWorkflow() {
  alive_->store(false);
}

Roe<void> GroupMembershipWorkflow::DissolveGroup(const std::string& group_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  std::string sibling_detail;
  {
    auto update = IGroupRosterStore::LockForUpdate();  // the roster we sign and the one we end, as one
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
    // The workflow goes with the messaging session (profile reset, account switch); a retry that outlives it does nothing.
    AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kRetryDelaysMs[attempt]),
                                           [this, attempt, alive = alive_]() {
                                             if (!alive->load()) {
                                               return;
                                             }
                                             AppRuntime::PostWorkerNormal([this, attempt, alive]() {
                                               if (alive->load()) {
                                                 ResumePendingGroupSends(attempt + 1);
                                               }
                                             });
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

Roe<void> GroupMembershipWorkflow::ChangeGroupSetting(
    const std::string& group_id, const GroupMembershipControlType type, const std::string& display,
    const std::function<Roe<bool>(GroupMetadata& metadata)>& apply,
    const std::function<Roe<std::string>(uint64_t roster_epoch)>& detail) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto update = IGroupRosterStore::LockForUpdate(); // the change and its epoch, as one
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (*local != metadata->owner_identity) {
    return Error("Only the group owner may change who does what in the group");
  }
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);
  }
  auto changed = apply(*metadata);
  if (!changed) {
    return changed.error();
  }
  if (!*changed) {
    return {}; // already so: nothing to sign or send
  }
  metadata->roster_epoch += 1;
  if (auto saved = roster_.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }
  update.unlock(); // not across the sends below
  auto encoded = detail(metadata->roster_epoch);
  if (!encoded) {
    return encoded.error();
  }
  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return Error("Group thread not found");
  }
  if (auto event = AppendMembershipSystemEvent((*thread)->id, type, display, *encoded, *local); !event) {
    return event.error();
  }
  return FanOutMembershipEvent(group_id, type, WithSignedRoster(group_id, *encoded), display, *local);
}

Roe<void> GroupMembershipWorkflow::SetMemberRole(const std::string& group_id, const std::string& member_identity,
                                                 const MemberRole role) {
  if (role == MemberRole::Owner) {
    return Error("Ownership moves by handing the group over, not by a role");
  }
  return ChangeGroupSetting(
      group_id, GroupMembershipControlType::MemberRoleChanged,
      role == MemberRole::Admin ? "Member made admin" : "Member no longer admin",
      [&](GroupMetadata& metadata) -> Roe<bool> {
        auto member = roster_.FindMember(group_id, member_identity);
        if (!member) {
          return member.error();
        }
        if (!*member) {
          return Error("Not a member of this group");
        }
        if (!GroupAllows(GroupAction::SetRole, MemberRole::Owner, metadata.policy.preset, (*member)->role)) {
          return Error("The owner's role is not changed this way");
        }
        if ((*member)->role == role) {
          return false;
        }
        GroupRosterMember updated = **member;
        updated.role = role;
        if (auto upserted = roster_.UpsertMember(group_id, updated); !upserted) {
          return upserted.error();
        }
        return true;
      },
      [&](const uint64_t epoch) {
        return GroupMembershipCodec::EncodeMemberRoleChanged({group_id, member_identity, role, epoch});
      });
}

Roe<void> GroupMembershipWorkflow::SetGroupPreset(const std::string& group_id, const GroupPreset preset) {
  return ChangeGroupSetting(
      group_id, GroupMembershipControlType::GroupPresetChanged,
      preset == GroupPreset::Managed ? "Group set to managed" : "Group set to open",
      [&](GroupMetadata& metadata) -> Roe<bool> {
        if (metadata.policy.preset == preset) {
          return false;
        }
        metadata.policy.preset = preset;
        return true;
      },
      [&](const uint64_t epoch) {
        return GroupMembershipCodec::EncodeGroupPresetChanged({group_id, preset, epoch});
      });
}

Roe<GroupPreset> GroupMembershipWorkflow::GroupPresetOf(const std::string& group_id) const {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  return metadata->policy.preset;
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
  if (metadata->sync_state == GroupSyncState::Dissolved) {
    return Error(group_roster_sync::kGroupDissolvedError);
  }
  if (*local != metadata->owner_identity) {
    return RenameAsMember(group_id, title); // G017: per the preset, as a signed change (G018)
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

Roe<void> GroupMembershipWorkflow::RenameAsMember(const std::string& group_id, const std::string& title) {
  auto local = LocalRelayIdentity();
  auto device = OwnDevice();
  if (!local || !device) {
    return !local ? local.error() : device.error();
  }
  auto draft =
      group_roster_sync::DraftRename(roster_, group_id, *local, *device, title, ChangeCards(), util::NowUnixMs());
  if (!draft) {
    return draft.error();
  }
  auto rename = CommitOwnChange(std::move(*draft));
  if (!rename) {
    return rename.error();
  }
  log().info << "group renamed by member group=" << group_id << " version=" << rename->title_version;
  return {};
}

std::optional<MemberRole> GroupMembershipWorkflow::LocalRole(const std::string& group_id) const {
  auto local = LocalRelayIdentity();
  auto metadata = roster_.LoadMetadata(group_id);
  if (!local || !metadata || !*metadata) {
    return std::nullopt;
  }
  if ((*metadata)->owner_identity == *local) {
    return MemberRole::Owner;
  }
  auto self = roster_.FindMember(group_id, *local);
  if (!self || !*self) {
    return std::nullopt;
  }
  return (*self)->role;
}

std::vector<std::string> GroupMembershipWorkflow::MembersAddedBy(const std::string& group_id,
                                                                 const std::string& account) const {
  std::vector<std::string> added;
  auto members = roster_.ListMembers(group_id);
  if (!members || account.empty()) {
    return added;
  }
  for (const GroupRosterMember& m : *members) {
    if (m.added_by == account && m.member_identity != account) {
      added.push_back(m.member_identity);
    }
  }
  return added;
}

std::vector<std::string> GroupMembershipWorkflow::AccountsRemovedBy(const std::string& group_id,
                                                                    const std::string& account) {
  return group_roster_sync::RemovedBy(roster_, group_id, account, ChangeCards(), util::NowUnixMs());
}

Roe<size_t> GroupMembershipWorkflow::RemoveEveryoneAddedBy(const std::string& group_id, const std::string& account) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const std::optional<MemberRole> role = LocalRole(group_id);
  if (!role || (*role != MemberRole::Owner && *role != MemberRole::Admin)) {
    return Error("Only the owner and admins remove people from the group");
  }
  size_t sent = 0;
  // One removal each (G020 6), as the owner or the admin we are.
  for (const std::string& identity : group_roster_sync::RemovableAddedBy(roster_, group_id, *local, account)) {
    if (auto removed = RemoveMemberInternal(group_id, identity); removed) {
      ++sent;
    } else {
      log().warning << "bulk remove skipped member=" << identity << ": " << removed.error().message;
    }
  }
  return sent;
}

Roe<size_t> GroupMembershipWorkflow::ReinviteEveryoneRemovedBy(const std::string& group_id,
                                                               const std::string& account) {
  if (LocalRole(group_id) != MemberRole::Owner) {
    return Error("Only the group owner re-invites everyone someone removed");
  }
  size_t sent = 0;
  for (const std::string& identity : AccountsRemovedBy(group_id, account)) {
    if (auto invited = InviteByIdentity(group_id, identity); invited) {
      ++sent;
    } else {
      log().warning << "bulk re-invite skipped account=" << identity << ": " << invited.error().message;
    }
  }
  return sent;
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

std::optional<Object> GroupMembershipWorkflow::KeptCard(const std::string& account_id) {
  auto kept = account_keys_.Kept(account_id);
  if (!kept || !*kept) {
    return std::nullopt;
  }
  return CarriedCardJson(**kept);
}

Roe<std::string> GroupMembershipWorkflow::JoinBundleDetail(GroupMembershipCodec::MemberJoinedPayload joined,
                                                           const GroupRosterStatement& statement,
                                                           const std::string& first) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  // The newcomer keys every member pairwise (docs/contracts/ACCOUNT_CARD.md); it takes a card only for someone the
  // roster it proves lists.
  size_t card_bytes = 0;
  for (GroupMembershipCodec::MemberJoinedEntry& entry : joined.members) {
    if (entry.member_identity == *local || entry.member_identity == joined.member_identity) {
      continue; // the newcomer has our keys and route from the invite's chat
    }
    if (auto route = mesh_messaging_.RelayRouteOf(entry.member_identity)) {
      entry.relay_user_id = *route;
    }
    auto card = KeptCard(entry.member_identity);
    if (!card) {
      continue;
    }
    card_bytes += DumpJson(*card).size();
    if (card_bytes <= kRosterCardsBudgetBytes) {
      entry.card = std::move(card);
    }
  }
  auto encoded = GroupMembershipCodec::EncodeMemberJoined(joined);
  if (!encoded) {
    return encoded.error();
  }
  auto with_statement = group_roster_sync_codec::WithStatement(*encoded, statement);
  if (!with_statement) {
    return with_statement.error();
  }
  // G018 item 6: every change it holds, all signed — the newcomer checks each and trusts no one, us included.
  return group_roster_sync_codec::WithChanges(*with_statement,
                                              group_roster_sync::ChangesForNewcomer(roster_, joined.group_id, first));
}

Roe<void> GroupMembershipWorkflow::SendJoinBundle(const std::string& group_id, const std::string& newcomer,
                                                  const std::string& invite_nonce, const std::string& added_by,
                                                  const std::string& first) {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  const std::optional<GroupRosterStatement> statement = group_roster_sync::Held(*metadata);
  if (!statement) {
    return Error("No owner statement to bring a newcomer in with");
  }
  auto roster = roster_.ListMembers(group_id);
  if (!roster) {
    return roster.error();
  }
  GroupMembershipCodec::MemberJoinedPayload joined;
  joined.group_id = group_id;
  joined.member_identity = newcomer;
  joined.roster_epoch = metadata->roster_epoch;
  joined.added_by = added_by;
  joined.invite_nonce = invite_nonce;
  for (const GroupRosterMember& row : *roster) {
    joined.members.push_back({.member_identity = row.member_identity, .role = row.role});
  }
  auto detail = JoinBundleDetail(joined, *statement, first);
  if (!detail) {
    return detail.error();
  }
  joined.members.clear();
  joined.invite_nonce.clear();
  auto plain = GroupMembershipCodec::EncodeMemberJoined(joined); // our copy keeps the line, not the bundle
  return SendMembershipDirectMessage(newcomer, GroupMembershipControlType::MemberJoined, *detail,
                                     "Member joined the group",
                                     plain ? std::optional<std::string>(*plain) : std::nullopt);
}

Roe<void> GroupMembershipWorkflow::PublishMemberJoined(const std::string& group_id, const std::string& member_identity,
                                                       const uint64_t roster_epoch, const std::string& invite_nonce) {
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
  payload.added_by = *local; // G019 5: "Alice added Carol"
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
  // The owner's statement of the roster with the newcomer in it (G012), signed once, rides both.
  auto statement = SignRoster(group_id);
  if (!statement) {
    log().warning << "Roster not signed group=" << group_id << ": " << statement.error().message;
    return statement.error();
  }
  // The members get the routes we know and the newcomer's card (they key each other pairwise).
  GroupMembershipCodec::MemberJoinedPayload for_members = payload;
  for (GroupMembershipCodec::MemberJoinedEntry& entry : for_members.members) {
    if (entry.member_identity == *local) {
      continue;
    }
    if (auto route = mesh_messaging_.RelayRouteOf(entry.member_identity)) {
      entry.relay_user_id = *route;
    }
    if (entry.member_identity == member_identity) {
      entry.card = KeptCard(member_identity);
    }
  }
  auto members_encoded = GroupMembershipCodec::EncodeMemberJoined(for_members);
  auto members_detail =
      members_encoded ? group_roster_sync_codec::WithStatement(*members_encoded, *statement) : members_encoded;
  // The newcomer gets the join bundle (G019), the same a member's invite sends.
  GroupMembershipCodec::MemberJoinedPayload for_newcomer = payload;
  for_newcomer.invite_nonce = invite_nonce;
  auto newcomer_detail = JoinBundleDetail(for_newcomer, *statement, {});
  if (!members_detail || !newcomer_detail) {
    return Error("member_joined: could not encode");
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
                                                            *members_detail, "Member joined the group", *plain);
                       });
  // Our copies keep the roster, not the cards (they are kept as cards on the other side).
  (void)SendMembershipDirectMessage(member_identity, GroupMembershipControlType::MemberJoined, *newcomer_detail,
                                    "Member joined the group", *plain);
  return {};
}

void GroupMembershipWorkflow::RefuseAdmit(const std::string& group_id, const std::string& invite_nonce,
                                          const std::string& member_identity, const std::string& reason) {
  (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Expired);
  if (auto refused = RefuseInvite(group_id, invite_nonce, member_identity, reason); !refused) {
    log().warning << "invite refusal not sent group=" << group_id << ": " << refused.error().message;
  }
}

Roe<void> GroupMembershipWorkflow::AdmitInvitee(const std::string& group_id, const std::string& invite_nonce,
                                                const std::string& member_identity) {
  auto update = IGroupRosterStore::LockForUpdate(); // the invite's check, the add and the invite's close, as one
  auto pending = roster_.LoadPendingInvite(invite_nonce);
  if (!pending || !*pending || !(*pending)->outbound || (*pending)->group_id != group_id ||
      (*pending)->invitee_identity != member_identity) {
    return Error("No invite of ours for this accept");
  }
  if ((*pending)->status == InviteStatus::Accepted) {
    return {}; // admitted once already
  }
  auto admitted = AdmitNewcomer(
      update,
      {.group_id = group_id, .member = member_identity, .invite_nonce = invite_nonce, .bundle_nonce = invite_nonce});
  if (!admitted && update.owns_lock()) { // no add went out (a failed send after it is not a refusal)
    update.unlock();
    const bool full = admitted.error().message == kGroupFullError;
    RefuseAdmit(group_id, invite_nonce, member_identity, full ? kInviteRefusedGroupFull : kInviteRefusedNotAllowed);
  }
  return admitted;
}

Roe<void> GroupMembershipWorkflow::AdmitByLink(const std::string& group_id, const std::string& link_id,
                                               const std::string& member_identity, const std::string& request_id) {
  auto update = IGroupRosterStore::LockForUpdate();
  return AdmitNewcomer(
      update, {.group_id = group_id, .member = member_identity, .link_id = link_id, .bundle_nonce = request_id});
}

Roe<void> GroupMembershipWorkflow::AdmitNewcomer(std::unique_lock<std::recursive_mutex>& update,
                                                 const Admission& admission) {
  const std::string& group_id = admission.group_id;
  const std::string& member_identity = admission.member;
  const bool by_invite = !admission.invite_nonce.empty();
  auto local = LocalRelayIdentity();
  auto device = OwnDevice();
  if (!local || !device) {
    return !local ? local.error() : device.error();
  }
  const int64_t now_ms = util::NowUnixMs();
  if (auto in = roster_.IsMember(group_id, member_identity); in && *in) {
    if (by_invite) {
      (void)roster_.UpdateInviteStatus(admission.invite_nonce, InviteStatus::Accepted); // a re-invite: resync them
    }
    update.unlock();
    return SendJoinBundle(group_id, member_identity, admission.bundle_nonce, {}, {});
  }
  auto draft = by_invite ? group_roster_sync::DraftAdd(roster_, group_id, *local, *device, member_identity,
                                                       admission.invite_nonce, ChangeCards(), now_ms)
                         : group_roster_sync::DraftLinkJoin(roster_, group_id, *local, *device, member_identity,
                                                            admission.link_id, ChangeCards(), now_ms);
  auto add = draft ? SignOwnChange(std::move(*draft)) : Roe<GroupChange>(draft.error());
  if (!add) {
    return add.error();
  }
  const std::string add_id = group_change::Id(*add);
  auto held = group_roster_sync::HoldChange(roster_, *add, ChangeCards(), now_ms, /*line_shown=*/true);
  if (!held || *held != group_roster_sync::ChangeHeld::Proved) {
    return held ? Error("The add does not prove itself on this device") : held.error();
  }
  auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms);
  if (!applied || !applied->view || !applied->view->Lists(member_identity)) {
    // It does not count here (R5: the group filled meanwhile): it goes nowhere, and the caller tells them.
    const GroupChangeVerdict* verdict = applied && applied->view ? applied->view->Verdict(add_id) : nullptr;
    const bool full = verdict && verdict->reason == GroupChangeReason::GroupFull;
    (void)roster_.DeleteGroupChanges(group_id, {add_id});
    (void)group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms);
    return Error(full ? std::string(kGroupFullError) : std::string(kGroupInviteNotAllowedError));
  }
  if (by_invite) {
    (void)roster_.UpdateInviteStatus(admission.invite_nonce, InviteStatus::Accepted);
  }
  (void)roster_.ExpireOutboundInvitesTo(group_id, member_identity);
  update.unlock(); // not across the sends below
  group_roster_sync_codec::ChangeNewcomer newcomer{
      .member_identity = member_identity,
      .card = KeptCard(member_identity),
      .relay_user_id = mesh_messaging_.RelayRouteOf(member_identity).value_or(std::string())};
  FanOutChange(group_id, *add, {}, newcomer);
  if (auto metadata = roster_.LoadMetadata(group_id); metadata && *metadata) {
    (void)group_change_lines::Append(store_, group_id, {*add}, (*metadata)->roster_epoch, *local);
    if ((*metadata)->owner_identity == *local) {
      ScheduleFold(group_id);
    }
  }
  log().info << "member added " << (by_invite ? "by invite" : "by link") << " group=" << group_id
             << " member=" << member_identity << (add->readmits.empty() ? "" : " (re-admitted)");
  return SendJoinBundle(group_id, member_identity, admission.bundle_nonce, *local, add_id);
}

Roe<GroupChange> GroupMembershipWorkflow::CommitOwnChange(GroupChange draft) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  const int64_t now_ms = util::NowUnixMs();
  auto change = SignOwnChange(std::move(draft));
  if (!change) {
    return change.error();
  }
  const std::string& group_id = change->group_id;
  auto held = group_roster_sync::HoldChange(roster_, *change, ChangeCards(), now_ms, /*line_shown=*/true);
  if (!held || *held != group_roster_sync::ChangeHeld::Proved) {
    return held ? Error("The change does not prove itself on this device") : held.error();
  }
  auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms);
  if (!applied) {
    return applied.error();
  }
  FanOutChange(group_id, *change);
  if (applied->new_title) {
    (void)group_change_lines::FollowTitle(store_, group_id, *applied->new_title);
  }
  if (auto metadata = roster_.LoadMetadata(group_id); metadata && *metadata) {
    (void)group_change_lines::Append(store_, group_id, {*change}, (*metadata)->roster_epoch, *local);
    if ((*metadata)->owner_identity == *local) {
      ScheduleFold(group_id);
    }
  }
  return change;
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
    parts.rows.push_back({m.member_identity, m.role, m.joined_at, m.added_by});
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
                                      std::move(parts.chain), /*dissolved=*/true, metadata.policy.preset);
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
  auto update = IGroupRosterStore::LockForUpdate();  // the roster read and the statement kept, as one
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
  // G018 item 4: every statement the owner signs folds the changes it holds (a leave, an admin's removal, ...).
  RosterParts parts = RosterPartsOf(*metadata, *members, handed);
  const int64_t now_ms = util::NowUnixMs();
  GroupRosterStatement draft = group_roster_sync::OwnerDraft(
      roster_, *metadata, std::move(parts.rows), std::move(parts.chain), *account, *key, ChangeCards(), now_ms);
  auto statement = group_roster_statement::SignStatement(identity_, *key, std::move(draft));
  if (!statement) {
    return statement.error();
  }
  if (statement->roster_epoch > metadata->roster_epoch) {
    metadata->roster_epoch = statement->roster_epoch;
    if (auto saved = roster_.UpsertMetadata(*metadata); !saved) {
      return saved.error();
    }
  }
  if (auto kept = group_roster_sync::KeepStatement(roster_, *statement); !kept) {
    return kept.error();
  }
  // What it folded is dropped here too; the rows are what it says.
  if (auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), now_ms); !applied) {
    return applied.error();
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
    // The owner answers with the roster as it is now: what it holds folded first, and sent to every member.
    (void)FoldNow(request.group_id);
    auto statement = SignRoster(request.group_id);
    if (!statement) {
      return statement.error();
    }
    reply.group_id = request.group_id;
    reply.request_id = request.request_id;
    reply.removed = !statement->Lists(asker);
    reply.statement = std::move(*statement);
    group_roster_sync::AddChangesTo(roster_, request, reply); // what it could not fold yet
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
             << " statement=" << (reply.statement ? reply.statement->roster_epoch : 0) << " removed=" << reply.removed
             << " changes=" << reply.changes.size() << (reply.more ? "+" : "");
  return SendRosterControl(asker, GroupMembershipControlType::GroupRosterReply,
                           group_roster_sync_codec::EncodeReply(reply), "Group members", *local);
}

void GroupMembershipWorkflow::ScheduleFold(const std::string& group_id) {
  {
    std::lock_guard lock(fold_mutex_);
    if (!folds_pending_.insert(group_id).second) {
      return; // one is on its way: it folds what arrives meanwhile too
    }
  }
  AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kFoldDelayMs), [this, group_id, alive = alive_]() {
    if (!alive->load()) {
      return;
    }
    AppRuntime::PostWorkerNormal([this, group_id, alive]() {
      if (!alive->load() || AppRuntime::IsShuttingDown()) {
        return;
      }
      {
        std::lock_guard lock(fold_mutex_);
        folds_pending_.erase(group_id);
      }
      if (auto folded = FoldNow(group_id); !folded) {
        log().warning << "fold failed group=" << group_id << ": " << folded.error().message;
      }
    });
  });
}

Roe<bool> GroupMembershipWorkflow::FoldNow(const std::string& group_id) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (metadata->owner_identity != *local ||
      !group_roster_sync::HoldsFoldable(roster_, group_id, ChangeCards(), util::NowUnixMs())) {
    return false;
  }
  if (auto sent = SignAndSendRoster(group_id); !sent) {
    return sent.error();
  }
  log().info << "changes folded group=" << group_id;
  return true;
}

void GroupMembershipWorkflow::FoldHeldChanges() {
  auto groups = roster_.ListGroupsWithChanges();
  if (!groups) {
    return;
  }
  for (const std::string& group_id : *groups) {
    if (auto folded = FoldNow(group_id); !folded) {
      log().warning << "fold failed group=" << group_id << ": " << folded.error().message;
    }
  }
}

Roe<void> GroupMembershipWorkflow::StartCatchUp(const std::string& group_id, const std::string& ask,
                                                const std::string& after) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  if (ask.empty() || ask == *local) {
    return {};
  }
  auto request = group_roster_sync::BeginCatchUp(roster_, group_id, util::NowUnixMs(), after);
  if (!request) {
    return request.error();
  }
  if (!*request) {
    return {}; // a check of its own runs, or the group is over
  }
  log().info << "Roster catch-up group=" << group_id << " asking=" << ask
             << " statement=" << (*request)->statement_epoch << (after.empty() ? "" : " (more)");
  return SendRosterControl(ask, GroupMembershipControlType::GroupRosterRequest,
                           group_roster_sync_codec::EncodeRequest(**request), "Checking group members", *local);
}

void GroupMembershipWorkflow::OnChangesApplied(const std::string& group_id, const std::vector<GroupChange>& changes) {
  auto local = LocalRelayIdentity();
  auto metadata = roster_.LoadMetadata(group_id);
  if (!local || !metadata || !*metadata) {
    return;
  }
  (void)group_change_lines::FollowTitle(store_, group_id, (*metadata)->title); // a rename among them (G017)
  if (auto lines = group_change_lines::Append(store_, group_id, changes, (*metadata)->roster_epoch, *local); !lines) {
    log().warning << "group change lines not written group=" << group_id << ": " << lines.error().message;
  }
  if ((*metadata)->owner_identity == *local) {
    ScheduleFold(group_id);
  }
}

void GroupMembershipWorkflow::RecheckChangesBy(const std::string& account) {
  for (const std::string& group_id : group_roster_sync::GroupsWithChangesBy(roster_, account)) {
    auto applied = group_roster_sync::ApplyView(roster_, group_id, ChangeCards(), util::NowUnixMs());
    if (!applied) {
      log().warning << "recheck failed group=" << group_id << ": " << applied.error().message;
      continue;
    }
    OnChangesApplied(group_id, applied->newly_applied);
  }
}

Roe<void> GroupMembershipWorkflow::StartRosterCheck(const std::string& group_id) {
  return StartRosterCheckAttempt(group_id, 0);
}

void GroupMembershipWorkflow::ScheduleRosterReask(const std::string& group_id, const std::string& request_id,
                                                  const size_t attempt) {
  if (attempt >= std::size(kRosterReaskDelaysMs)) {
    return; // a member's next group message, or the next start, still asks again (G013)
  }
  // The workflow goes with the messaging session (profile reset, account switch); a re-ask that outlives it does nothing.
  AppRuntime::ScheduleCoordinatorOneShot(
      std::chrono::milliseconds(kRosterReaskDelaysMs[attempt]),
      [this, group_id, request_id, attempt, alive = alive_]() {
        if (!alive->load()) {
          return;
        }
        AppRuntime::PostWorkerNormal([this, group_id, request_id, attempt, alive]() {
          if (!alive->load() || AppRuntime::IsShuttingDown()) {
            return;
          }
          auto metadata = roster_.LoadMetadata(group_id);
          if (!metadata || !*metadata || !group_roster_sync::StillAwaiting(**metadata, request_id)) {
            return; // answered, or a newer check runs with its own re-asks
          }
          log().info << "Roster check unanswered group=" << group_id << ", asking again";
          (void)StartRosterCheckAttempt(group_id, attempt + 1);
        });
      });
}

Roe<void> GroupMembershipWorkflow::StartRosterCheckAttempt(const std::string& group_id, const size_t attempt) {
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
  // To everyone asked at once (V058 2): each copy sealed for its pair, off this thread (not live: the normal lane).
  std::vector<ControlSend> sends;
  for (const std::string& identity : check->ask) {
    auto control = GroupMembershipCodec::BuildSystemMessage(
        std::string(), GroupMembershipControlType::GroupRosterRequest, "Checking group members", detail, *local);
    if (control) {
      // With our card: the asker may be a restored device its members do not know yet (G013).
      sends.push_back(ControlSend{.peer_identity = identity, .control = std::move(*control), .with_card = true});
    }
  }
  mesh_messaging_.SendControlToMany(
      std::move(sends),
      [this, alive = alive_, group_id](std::vector<ControlSendResult> results) {
        if (!alive->load()) {
          return;
        }
        for (const ControlSendResult& result : results) {
          if (!result.queued) {
            log().info << "roster check not sent group=" << group_id << " to=" << result.peer_identity
                       << " err=" << result.queued.error().message;
          }
        }
      },
      WorkerLane::Normal);
  ScheduleRosterReask(group_id, check->request.request_id, attempt);
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
  return mesh_messaging_.SendControl(peer_identity, *control, std::nullopt, false, true);
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
  auto update = IGroupRosterStore::LockForUpdate();
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
