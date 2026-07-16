#include "feature/messaging/GroupMembershipService.h"

#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/SendRelayOptions.h"

#include "common/Utilities.h"

namespace pbr {

GroupMembershipService::GroupMembershipService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                               GroupRosterStore& roster, GroupInviteGate& invite_gate,
                                               P2pMessagingService& p2p)
    : store_(store), contacts_(contacts), identity_(identity), roster_(roster), invite_gate_(invite_gate), p2p_(p2p) {}

void GroupMembershipService::SetInboundPolicy(const GroupInvitePolicy policy) {
  invite_gate_.SetInboundPolicy(policy);
}

Roe<std::string> GroupMembershipService::LocalRelayIdentity() const {
  auto id = identity_.Get();
  if (!id) {
    return id.error();
  }
  if (id->relay_user_id.empty()) {
    return Error("Local relay identity unavailable");
  }
  return id->relay_user_id;
}

Roe<std::string> GroupMembershipService::ResolveRelayIdentity(const std::string& contact_id) const {
  auto contact = contacts_.Get(contact_id);
  if (!contact) {
    return contact.error();
  }
  if (!*contact) {
    return Error("Contact not found");
  }
  const DirectChatTarget target = DirectChatTargetFromContact(**contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return Error("Contact has no routable relay identity");
  }
  return target.peer_identity_value;
}

Roe<GroupMetadata> GroupMembershipService::LoadMetadataOrError(const std::string& group_id) const {
  auto metadata = roster_.LoadMetadata(group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!metadata->has_value()) {
    return Error("Group metadata not found");
  }
  return **metadata;
}

Roe<void> GroupMembershipService::AppendMembershipSystemEvent(const std::string& thread_id,
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

Roe<void> GroupMembershipService::SendInviteDirectMessage(const GroupInvitePayload& invite,
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
  auto sys = GroupMembershipCodec::BuildSystemMessage(thread->id, GroupMembershipControlType::GroupInvite,
                                                      "Group invitation: " + invite.group_title, *detail, *local);
  if (!sys) {
    return sys.error();
  }
  sys->chat_actions = GroupMembershipCodec::BuildInviteChatActions(invite);
  sys->relay_visible = true;
  if (auto appended = store_.AppendMessage(*sys); !appended) {
    return appended.error();
  }
  SendRelayOptions opts;
  opts.generation = "system";
  auto sent = p2p_.SendUserMessage(thread->id, "Group invitation: " + invite.group_title, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<Thread> GroupMembershipService::CreateGroup(const std::string& title,
                                                const std::vector<std::string>& member_contact_ids) {
  auto local_identity = LocalRelayIdentity();
  if (!local_identity) {
    return local_identity.error();
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

Roe<void> GroupMembershipService::InviteMember(const std::string& group_id, const std::string& invitee_contact_id) {
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
  if (!invite_gate_.CanSendInvite(MemberRole::Owner)) {
    return Error("Actor lacks invite permission");
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
  invite.expires_at = util::NowUnixMs() + 7LL * 24 * 60 * 60 * 1000;
  invite.actor_role = MemberRole::Owner;

  PendingGroupInvite pending;
  pending.invite_nonce = invite.invite_nonce;
  pending.group_id = group_id;
  pending.inviter_identity = invite.inviter_identity;
  pending.invitee_identity = invite.invitee_identity;
  pending.status = InviteStatus::Pending;
  pending.expires_at = invite.expires_at;
  pending.created_at = util::NowUnixMs();
  if (auto recorded = invite_gate_.RecordPendingInvite(pending); !recorded) {
    return recorded.error();
  }
  return SendInviteDirectMessage(invite, invitee_contact_id);
}

Roe<void> GroupMembershipService::HandleInboundInvitePayload(const GroupInvitePayload& invite) {
  if (!invite_gate_.AllowsInboundInvite(invite)) {
    return Error("Invite blocked by policy");
  }
  PendingGroupInvite pending;
  pending.invite_nonce = invite.invite_nonce;
  pending.group_id = invite.group_id;
  pending.inviter_identity = invite.inviter_identity;
  pending.invitee_identity = invite.invitee_identity;
  pending.status = InviteStatus::Pending;
  pending.expires_at = invite.expires_at;
  pending.created_at = util::NowUnixMs();
  return roster_.UpsertPendingInvite(pending);
}

Roe<Thread> GroupMembershipService::AcceptInvite(const std::string& invite_nonce) {
  auto pending = roster_.LoadPendingInvite(invite_nonce);
  if (!pending || !pending->has_value()) {
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

  auto metadata = roster_.LoadMetadata((*pending)->group_id);
  std::string title = metadata && metadata->has_value() ? metadata->value().title : "Group chat";

  auto thread = store_.FindOrCreateGroupThread((*pending)->group_id, title, {});
  if (!thread) {
    return thread.error();
  }

  GroupRosterMember member;
  member.member_identity = *local;
  member.role = MemberRole::Member;
  member.joined_at = util::NowUnixMs();
  if (auto upsert = roster_.UpsertMember((*pending)->group_id, member); !upsert) {
    return upsert.error();
  }
  (void)roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Accepted);

  uint64_t roster_epoch = metadata && metadata->has_value() ? metadata->value().roster_epoch : 1;
  auto detail = GroupMembershipCodec::EncodeMemberJoined((*pending)->group_id, *local, MemberRole::Member, roster_epoch);
  if (!detail) {
    return detail.error();
  }
  if (auto event = AppendMembershipSystemEvent(thread->id, GroupMembershipControlType::MemberJoined,
                                               "You joined the group", *detail, *local);
      !event) {
    return event.error();
  }
  return *thread;
}

Roe<void> GroupMembershipService::DeclineInvite(const std::string& invite_nonce) {
  return roster_.UpdateInviteStatus(invite_nonce, InviteStatus::Declined);
}

Roe<void> GroupMembershipService::RemoveMember(const std::string& group_id, const std::string& member_contact_id) {
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
  auto member_identity = ResolveRelayIdentity(member_contact_id);
  if (!member_identity) {
    return member_identity.error();
  }
  if (*member_identity == *local) {
    return Error("Owner cannot remove self — transfer ownership or delete group");
  }

  if (auto removed = roster_.RemoveMember(group_id, *member_identity); !removed) {
    return removed.error();
  }
  metadata->roster_epoch += 1;
  (void)roster_.UpsertMetadata(*metadata);
  (void)roster_.BumpGroupSessionEpoch(group_id, metadata->session_epoch + 1);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return Error("Group thread not found");
  }
  auto detail = GroupMembershipCodec::EncodeMemberRemoved(group_id, *member_identity, metadata->roster_epoch);
  if (!detail) {
    return detail.error();
  }
  return AppendMembershipSystemEvent((*thread)->id, GroupMembershipControlType::MemberRemoved, "Member removed",
                                     *detail, *local);
}

Roe<void> GroupMembershipService::LeaveGroup(const std::string& group_id) {
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
  (void)roster_.RemoveMember(group_id, *local);
  metadata->roster_epoch += 1;
  (void)roster_.UpsertMetadata(*metadata);

  auto thread = store_.FindGroupThread(group_id);
  if (!thread || !*thread) {
    return Error("Group thread not found");
  }
  auto detail = GroupMembershipCodec::EncodeMemberLeft(group_id, *local, metadata->roster_epoch);
  if (!detail) {
    return detail.error();
  }
  return AppendMembershipSystemEvent((*thread)->id, GroupMembershipControlType::MemberLeft, "Member left", *detail,
                                     *local);
}

Roe<Thread> GroupMembershipService::ForkGroup(const std::string& group_id, const std::string& new_title,
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

Roe<std::vector<GroupRosterMember>> GroupMembershipService::ListRoster(const std::string& group_id) const {
  return roster_.ListMembers(group_id);
}

} // namespace pbr
