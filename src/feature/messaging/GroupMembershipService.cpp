#include "feature/messaging/GroupMembershipService.h"

#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupTypes.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/people/ContactJson.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <set>

namespace pbr {

GroupMembershipService::GroupMembershipService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                                               GroupRosterStore& roster, GroupInviteGate& invite_gate,
                                               P2pMessagingService& p2p)
    : store_(store), contacts_(contacts), identity_(identity), roster_(roster), invite_gate_(invite_gate), p2p_(p2p) {
  redirectLogger("GroupMembershipService");
}

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
  // Send as a system ChatPayload on the wire (same shape as group_renamed). Do not attach
  // Accept/Decline locally — those are for the invitee, hydrated on inbound ingest.
  const std::string display = "Group invitation: " + invite.group_title;
  const std::string payload_json =
      nlohmann::json({{"control_type", GroupMembershipControlTypeToWire(GroupMembershipControlType::GroupInvite)},
                      {"detail", *detail}})
          .dump();
  SendRelayOptions opts;
  opts.generation = "system";
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_json;
  opts.sender_contact_id = *local;
  auto sent = p2p_.SendUserMessage(thread->id, display, opts);
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
  pending.group_title = invite.group_title;
  pending.inviter_identity = invite.inviter_identity;
  pending.invitee_identity = invite.invitee_identity;
  pending.roster_epoch = invite.roster_epoch;
  pending.status = InviteStatus::Pending;
  pending.expires_at = invite.expires_at;
  pending.created_at = util::NowUnixMs();
  if (auto recorded = invite_gate_.RecordPendingInvite(pending); !recorded) {
    return recorded.error();
  }
  return SendInviteDirectMessage(invite, invitee_contact_id);
}

Roe<void> GroupMembershipService::SendInviteResponseDirectMessage(const std::string& inviter_identity,
                                                                  const std::string& invite_nonce,
                                                                  const std::string& group_id,
                                                                  const GroupMembershipControlType response_type) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  direct_target.peer_identity_value = inviter_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = inviter_identity;
  if (auto contact = contacts_.FindByIdentity(inviter_identity, ContactIdKind::RelayUser)) {
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
  const std::string payload_json =
      nlohmann::json({{"control_type", GroupMembershipControlTypeToWire(response_type)}, {"detail", *detail}}).dump();
  SendRelayOptions opts;
  opts.generation = "system";
  opts.update_preview = false;
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_json;
  opts.sender_contact_id = *local;
  auto sent = p2p_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
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

Roe<void> GroupMembershipService::DeclineInvite(const std::string& invite_nonce) {
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

Roe<void> GroupMembershipService::DismissLocalGroup(const std::string& group_id) {
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

Roe<void> GroupMembershipService::ResolveInviteCard(const std::string& inviter_identity,
                                                    const std::string& invite_nonce, const InviteStatus status,
                                                    const std::string& status_text) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
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

Roe<void> GroupMembershipService::SendMembershipDirectMessage(const std::string& peer_identity,
                                                              const GroupMembershipControlType control_type,
                                                              const std::string& detail_json,
                                                              const std::string& display) {
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  direct_target.peer_identity_value = peer_identity;
  direct_target.channel = ThreadChannel::E2ePublic;

  std::string contact_id;
  std::string dm_title = peer_identity;
  if (auto contact = contacts_.FindByIdentity(peer_identity, ContactIdKind::RelayUser)) {
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
  const std::string payload_json =
      nlohmann::json({{"control_type", GroupMembershipControlTypeToWire(control_type)}, {"detail", detail_json}})
          .dump();
  SendRelayOptions opts;
  opts.generation = "system";
  opts.update_preview = false;
  opts.content_type = ChatContentType::System;
  opts.payload_json = payload_json;
  opts.sender_contact_id = *local;
  auto sent = p2p_.SendUserMessage(thread->id, display, opts);
  if (!sent) {
    return sent.error();
  }
  return {};
}

Roe<void> GroupMembershipService::FanOutMembershipEvent(const std::string& group_id,
                                                         const GroupMembershipControlType control_type,
                                                         const std::string& detail_json, const std::string& display,
                                                         const std::string& skip_identity,
                                                         const std::optional<std::string>& also_notify_identity) {
  auto members = roster_.ListMembers(group_id);
  if (!members) {
    return members.error();
  }
  std::set<std::string> notified;
  for (const GroupRosterMember& member : *members) {
    if (member.member_identity == skip_identity) {
      continue;
    }
    notified.insert(member.member_identity);
    if (auto sent = SendMembershipDirectMessage(member.member_identity, control_type, detail_json, display); !sent) {
      return sent.error();
    }
  }
  if (also_notify_identity && !also_notify_identity->empty() &&
      notified.find(*also_notify_identity) == notified.end() && *also_notify_identity != skip_identity) {
    (void)SendMembershipDirectMessage(*also_notify_identity, control_type, detail_json, display);
  }
  return {};
}

Roe<void> GroupMembershipService::RemoveMemberInternal(const std::string& group_id,
                                                        const std::string& member_identity) {
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
  if (member_identity == *local) {
    return Error("Owner cannot remove self — transfer ownership or leave");
  }

  if (auto removed = roster_.RemoveMember(group_id, member_identity); !removed) {
    return removed.error();
  }
  metadata->roster_epoch += 1;
  (void)roster_.UpsertMetadata(*metadata);
  (void)roster_.BumpGroupSessionEpoch(group_id, metadata->session_epoch + 1);
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
  return FanOutMembershipEvent(group_id, GroupMembershipControlType::MemberRemoved, *detail, "Member removed",
                               *local, member_identity);
}

Roe<void> GroupMembershipService::RemoveMember(const std::string& group_id, const std::string& member_contact_id) {
  auto member_identity = ResolveRelayIdentity(member_contact_id);
  if (!member_identity) {
    return member_identity.error();
  }
  return RemoveMemberInternal(group_id, *member_identity);
}

Roe<void> GroupMembershipService::RemoveMemberByIdentity(const std::string& group_id,
                                                         const std::string& member_identity) {
  if (member_identity.empty()) {
    return Error("member_identity required");
  }
  return RemoveMemberInternal(group_id, member_identity);
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

  metadata->roster_epoch += 1;
  auto detail = GroupMembershipCodec::EncodeMemberLeft(group_id, *local, metadata->roster_epoch);
  if (!detail) {
    return detail.error();
  }
  // Best-effort notify remaining members; local leave always proceeds so close cannot get stuck.
  (void)FanOutMembershipEvent(group_id, GroupMembershipControlType::MemberLeft, *detail, "Member left", *local);

  (void)roster_.RemoveMember(group_id, *local);
  (void)roster_.UpsertMetadata(*metadata);
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

Roe<void> GroupMembershipService::LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity) {
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
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

  metadata->roster_epoch += 1;
  metadata->owner_identity = new_owner_identity;
  GroupRosterMember successor;
  successor.member_identity = new_owner_identity;
  successor.role = MemberRole::Owner;
  successor.joined_at = util::NowUnixMs();
  if (auto upserted = roster_.UpsertMember(group_id, successor); !upserted) {
    return upserted.error();
  }
  if (auto removed = roster_.RemoveMember(group_id, *local); !removed) {
    return removed.error();
  }
  if (auto saved = roster_.UpsertMetadata(*metadata); !saved) {
    return saved.error();
  }

  auto detail =
      GroupMembershipCodec::EncodeOwnerTransferred(group_id, new_owner_identity, metadata->roster_epoch, true);
  if (!detail) {
    return detail.error();
  }
  (void)FanOutMembershipEvent(group_id, GroupMembershipControlType::OwnerTransferred, *detail,
                              "Group ownership transferred", *local);
  (void)roster_.ClearGroupTarget(group_id);
  return {};
}

void GroupMembershipService::MarkMemberUnreachable(const std::string& group_id, const std::string& member_identity) {
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

void GroupMembershipService::ClearMemberUnreachable(const std::string& group_id,
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

bool GroupMembershipService::IsMemberUnreachable(const std::string& group_id,
                                                 const std::string& member_identity) const {
  std::lock_guard lock(unreachable_mutex_);
  return unreachable_.find({group_id, member_identity}) != unreachable_.end();
}

std::vector<std::string> GroupMembershipService::ListUnreachable(const std::string& group_id) const {
  std::vector<std::string> out;
  std::lock_guard lock(unreachable_mutex_);
  for (const auto& entry : unreachable_) {
    if (entry.first == group_id) {
      out.push_back(entry.second);
    }
  }
  return out;
}

bool GroupMembershipService::IsOwnerUnreachable(const std::string& group_id) const {
  auto metadata = roster_.LoadMetadata(group_id);
  if (!metadata || !*metadata) {
    return false;
  }
  return IsMemberUnreachable(group_id, (**metadata).owner_identity);
}

Roe<void> GroupMembershipService::EnsureOwnerUnreachableAdvisory(const std::string& group_id) {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
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
  const std::string detail =
      nlohmann::json({{"group_id", group_id}, {"owner_identity", metadata->owner_identity}}).dump();
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

Roe<void> GroupMembershipService::ResolveOwnerUnreachableAdvisory(const std::string& group_id) {
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

Roe<Thread> GroupMembershipService::OpenOwnerDirectMessage(const std::string& owner_identity) {
  if (owner_identity.empty()) {
    return Error("owner_identity required");
  }
  DirectChatTarget direct_target;
  direct_target.peer_identity_kind = ContactIdKindToString(ContactIdKind::RelayUser);
  direct_target.peer_identity_value = owner_identity;
  direct_target.channel = ThreadChannel::E2ePublic;
  std::string contact_id;
  std::string title = owner_identity;
  if (auto contact = contacts_.FindByIdentity(owner_identity, ContactIdKind::RelayUser)) {
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

Roe<void> GroupMembershipService::SendRenameDirectMessage(const std::string& member_identity,
                                                          const std::string& group_id, const std::string& title,
                                                          const uint64_t roster_epoch) {
  auto detail = GroupMembershipCodec::EncodeGroupRenamed(group_id, title, roster_epoch);
  if (!detail) {
    return detail.error();
  }
  return SendMembershipDirectMessage(member_identity, GroupMembershipControlType::GroupRenamed, *detail,
                                     "Group renamed to " + title);
}

Roe<void> GroupMembershipService::RenameGroupShared(const std::string& group_id, const std::string& title) {
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
  if (!RoleHasPermission(MemberRole::Owner, kPermRename)) {
    return Error("Actor lacks rename permission");
  }

  metadata->title = title;
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
    auto detail = GroupMembershipCodec::EncodeGroupRenamed(group_id, title, metadata->roster_epoch);
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
    if (auto sent = SendRenameDirectMessage(member.member_identity, group_id, title, metadata->roster_epoch); !sent) {
      return sent.error();
    }
  }
  return {};
}

Roe<void> GroupMembershipService::ApplyInboundGroupRenamed(const GroupMembershipCodec::GroupRenamedPayload& payload,
                                                           const std::string& actor_identity) {
  auto metadata = roster_.LoadMetadata(payload.group_id);
  if (!metadata) {
    return metadata.error();
  }
  if (!*metadata) {
    return Error("Unknown group for rename");
  }
  if (!actor_identity.empty() && (*metadata)->owner_identity != actor_identity) {
    return Error("Rename rejected: actor is not the group owner");
  }
  GroupMetadata updated = **metadata;
  updated.title = payload.title;
  if (payload.roster_epoch > updated.roster_epoch) {
    updated.roster_epoch = payload.roster_epoch;
  }
  if (auto saved = roster_.UpsertMetadata(updated); !saved) {
    return saved.error();
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
    auto detail = GroupMembershipCodec::EncodeGroupRenamed(payload.group_id, payload.title, updated.roster_epoch);
    if (detail) {
      (void)AppendMembershipSystemEvent(row.id, GroupMembershipControlType::GroupRenamed,
                                        "Group renamed to " + payload.title, *detail, actor_identity);
    }
  }
  return {};
}

Roe<std::vector<GroupRosterMember>> GroupMembershipService::ListRoster(const std::string& group_id) const {
  return roster_.ListMembers(group_id);
}

Roe<bool> GroupMembershipService::IsLocalOwner(const std::string& group_id) const {
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

Roe<std::string> GroupMembershipService::OwnerIdentity(const std::string& group_id) const {
  auto metadata = LoadMetadataOrError(group_id);
  if (!metadata) {
    return metadata.error();
  }
  return metadata->owner_identity;
}

} // namespace pbr
