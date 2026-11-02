#include "lib/messaging/testing/InMemoryMessagingStores.h"

#include "lib/messaging/GroupMembershipCodec.h"

#include <algorithm>

namespace pbr {

InMemoryGroupRosterStore::InMemoryGroupRosterStore(InMemoryDeliveryStore& threads) : threads_(threads) {
  // Where groups are kept, group events in a direct chat are not the chat's to show (as the app's storage).
  threads_.SetHiddenInDirectThread(&GroupMembershipCodec::IsHiddenInDirectThread);
}

void InMemoryGroupRosterStore::SetOnChanged(std::function<void(const GroupRosterChange& change)> on_changed) {
  on_changed_ = std::move(on_changed);
}

void InMemoryGroupRosterStore::NotifyChanged(GroupRosterChange change) const {
  if (on_changed_) {
    on_changed_(change);
  }
}

Roe<void> InMemoryGroupRosterStore::UpsertMetadata(const GroupMetadata& metadata) const {
  {
    std::lock_guard lock(mutex_);
    metadata_[metadata.group_id] = metadata;
  }
  NotifyChanged({.group_id = metadata.group_id});
  return {};
}

Roe<std::optional<GroupMetadata>> InMemoryGroupRosterStore::LoadMetadata(const std::string& group_id) const {
  std::lock_guard lock(mutex_);
  auto metadata = metadata_.find(group_id);
  if (metadata == metadata_.end()) {
    return std::optional<GroupMetadata>();
  }
  return std::optional<GroupMetadata>(metadata->second);
}

Roe<std::vector<std::string>> InMemoryGroupRosterStore::ListGroupIds() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> ids;
  for (const auto& [id, metadata] : metadata_) {
    ids.push_back(id);
  }
  return ids;
}

Roe<void> InMemoryGroupRosterStore::UpsertMember(const std::string& group_id, const GroupRosterMember& member) const {
  {
    std::lock_guard lock(mutex_);
    rosters_[group_id][member.member_identity] = member;
  }
  NotifyChanged({.group_id = group_id});
  return {};
}

Roe<void> InMemoryGroupRosterStore::RemoveMember(const std::string& group_id,
                                                 const std::string& member_identity) const {
  bool removed = false;
  {
    std::lock_guard lock(mutex_);
    if (auto roster = rosters_.find(group_id); roster != rosters_.end()) {
      removed = roster->second.erase(member_identity) > 0;
    }
  }
  NotifyChanged({.group_id = group_id, .member_gone = removed ? member_identity : std::string{}});
  return {};
}

Roe<std::vector<GroupRosterMember>> InMemoryGroupRosterStore::ListMembers(const std::string& group_id) const {
  std::lock_guard lock(mutex_);
  std::vector<GroupRosterMember> members;
  if (auto roster = rosters_.find(group_id); roster != rosters_.end()) {
    for (const auto& [identity, member] : roster->second) {
      members.push_back(member);
    }
  }
  return members;
}

Roe<std::optional<GroupRosterMember>> InMemoryGroupRosterStore::FindMember(const std::string& group_id,
                                                                           const std::string& member_identity) const {
  std::lock_guard lock(mutex_);
  if (auto roster = rosters_.find(group_id); roster != rosters_.end()) {
    if (auto member = roster->second.find(member_identity); member != roster->second.end()) {
      return std::optional<GroupRosterMember>(member->second);
    }
  }
  return std::optional<GroupRosterMember>();
}

Roe<bool> InMemoryGroupRosterStore::IsMember(const std::string& group_id, const std::string& member_identity) const {
  auto member = FindMember(group_id, member_identity);
  if (!member) {
    return member.error();
  }
  return member->has_value();
}

Roe<void> InMemoryGroupRosterStore::UpsertPendingInvite(const PendingGroupInvite& invite) const {
  std::lock_guard lock(mutex_);
  invites_.try_emplace(invite.invite_nonce, invite); // as SQLite: an invite once stored stays as it was
  return {};
}

Roe<std::optional<PendingGroupInvite>> InMemoryGroupRosterStore::LoadPendingInvite(
    const std::string& invite_nonce) const {
  std::lock_guard lock(mutex_);
  auto invite = invites_.find(invite_nonce);
  if (invite == invites_.end()) {
    return std::optional<PendingGroupInvite>();
  }
  return std::optional<PendingGroupInvite>(invite->second);
}

Roe<void> InMemoryGroupRosterStore::UpdateInviteStatus(const std::string& invite_nonce, InviteStatus status) const {
  std::string group_id;
  {
    std::lock_guard lock(mutex_);
    auto invite = invites_.find(invite_nonce);
    if (invite == invites_.end()) {
      return {};
    }
    invite->second.status = status;
    group_id = invite->second.group_id;
  }
  NotifyChanged({.group_id = group_id});
  return {};
}

Roe<size_t> InMemoryGroupRosterStore::CountPendingInvitesSince(int64_t since_ms) const {
  std::lock_guard lock(mutex_);
  return static_cast<size_t>(std::count_if(invites_.begin(), invites_.end(), [&](const auto& entry) {
    const PendingGroupInvite& invite = entry.second;
    return invite.status == InviteStatus::Pending && !invite.outbound && invite.created_at >= since_ms;
  }));
}

Roe<void> InMemoryGroupRosterStore::ExpireOutboundInvitesTo(const std::string& group_id,
                                                            const std::string& invitee_identity) const {
  std::lock_guard lock(mutex_);
  for (auto& [nonce, invite] : invites_) {
    if (invite.group_id == group_id && invite.invitee_identity == invitee_identity && invite.outbound &&
        invite.status == InviteStatus::Pending) {
      invite.status = InviteStatus::Expired;
    }
  }
  return {};
}

std::vector<PendingGroupInvite> InMemoryGroupRosterStore::VoidPendingInvitesLocked(const std::string& group_id) const {
  std::vector<PendingGroupInvite> voided;
  for (auto& [nonce, invite] : invites_) {
    if (invite.group_id == group_id && invite.status == InviteStatus::Pending) {
      voided.push_back(invite);
      invite.status = InviteStatus::Expired;
    }
  }
  return voided;
}

Roe<std::vector<PendingGroupInvite>> InMemoryGroupRosterStore::ExpirePendingInvitesOf(
    const std::string& group_id) const {
  std::lock_guard lock(mutex_);
  return VoidPendingInvitesLocked(group_id);
}

Roe<std::optional<std::vector<PendingGroupInvite>>> InMemoryGroupRosterStore::EndGroup(
    const std::string& group_id, const GroupEnding& ending) const {
  std::vector<PendingGroupInvite> voided;
  {
    std::lock_guard lock(mutex_);
    auto metadata = metadata_.find(group_id);
    if (metadata == metadata_.end() || metadata->second.sync_state == GroupSyncState::Dissolved) {
      return std::optional<std::vector<PendingGroupInvite>>();
    }
    voided = VoidPendingInvitesLocked(group_id);
    rosters_.erase(group_id);
    threads_.ClearGroupTarget(group_id);
    GroupMetadata& ended = metadata->second;
    ended.roster_epoch = std::max(ended.roster_epoch, ending.roster_epoch);
    ended.owner_identity = ending.owner_identity;
    ended.sync_state = GroupSyncState::Dissolved;
    ended.sync_request_id.clear();
    ended.sync_requested_at = 0;
    ended.roster_statement_json = ending.statement_json;
    for (PendingGroupSend send : ending.sends) {
      send.group_id = group_id;
      sends_[{group_id, send.recipient, send.control_type}] = send;
    }
  }
  NotifyChanged({.group_id = group_id, .ended = true});
  return std::optional<std::vector<PendingGroupInvite>>(std::move(voided));
}

Roe<std::vector<PendingGroupSend>> InMemoryGroupRosterStore::ListPendingSends(const std::string& group_id) const {
  std::lock_guard lock(mutex_);
  std::vector<PendingGroupSend> sends; // ordered by group, then recipient (the map's key order)
  for (const auto& [key, send] : sends_) {
    if (group_id.empty() || std::get<0>(key) == group_id) {
      sends.push_back(send);
    }
  }
  return sends;
}

Roe<void> InMemoryGroupRosterStore::DeletePendingSend(const PendingGroupSend& send) const {
  std::lock_guard lock(mutex_);
  sends_.erase({send.group_id, send.recipient, send.control_type});
  return {};
}

Roe<std::vector<PendingGroupInvite>> InMemoryGroupRosterStore::ListOutboundPendingInvites(const std::string& group_id,
                                                                                          int64_t now_ms) const {
  std::lock_guard lock(mutex_);
  std::vector<PendingGroupInvite> live;
  for (const auto& [nonce, invite] : invites_) {
    if (invite.group_id == group_id && invite.outbound && invite.status == InviteStatus::Pending &&
        (!invite.expires_at || *invite.expires_at > now_ms)) {
      live.push_back(invite);
    }
  }
  std::stable_sort(live.begin(), live.end(), [](const PendingGroupInvite& a, const PendingGroupInvite& b) {
    return a.created_at > b.created_at;
  });
  std::vector<PendingGroupInvite> latest; // the newest one per invitee
  for (const PendingGroupInvite& invite : live) {
    const bool seen = std::any_of(latest.begin(), latest.end(), [&](const PendingGroupInvite& other) {
      return other.invitee_identity == invite.invitee_identity;
    });
    if (!seen) {
      latest.push_back(invite);
    }
  }
  return latest;
}

Roe<void> InMemoryGroupRosterStore::UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id,
                                                      uint32_t session_epoch, uint64_t next_outgoing_seq) const {
  threads_.SetGroupTarget(group_id, local_thread_id, session_epoch, next_outgoing_seq);
  return {};
}

Roe<void> InMemoryGroupRosterStore::ClearGroupTarget(const std::string& group_id) const {
  threads_.ClearGroupTarget(group_id);
  return {};
}

Roe<void> InMemoryGroupRosterStore::ClearGroupTargetByThreadId(const std::string& local_thread_id) const {
  threads_.ClearGroupTargetsOfThread(local_thread_id);
  return {};
}

Roe<std::optional<std::string>> InMemoryGroupRosterStore::FindThreadIdForGroup(const std::string& group_id) const {
  return threads_.GroupTargetThread(group_id);
}

Roe<uint64_t> InMemoryGroupRosterStore::AllocateGroupSenderSeq(const std::string& group_id) const {
  return threads_.AllocateGroupSeq(group_id);
}

Roe<uint64_t> InMemoryGroupRosterStore::AllocateMemberSeq(const std::string& group_id,
                                                          const std::string& member_identity, uint32_t pair_epoch,
                                                          const std::string& member_device) const {
  // A device pair counts on its own: its counter is the member's with the device (M021).
  const std::string counter = member_device.empty() ? member_identity : member_identity + "/" + member_device;
  std::lock_guard lock(mutex_);
  MemberSeq& seq = member_seqs_[{group_id, counter}];
  if (pair_epoch > seq.session_epoch) {
    seq.session_epoch = pair_epoch;
    seq.next_seq = 1; // a newer pair epoch counts from 1
  }
  return seq.next_seq++;
}

Roe<uint32_t> InMemoryGroupRosterStore::GetGroupSessionEpoch(const std::string& group_id) const {
  return threads_.GroupEpoch(group_id);
}

Roe<void> InMemoryGroupRosterStore::BumpGroupSessionEpoch(const std::string& group_id, uint32_t new_epoch) const {
  return threads_.SetGroupEpoch(group_id, new_epoch);
}

namespace {

bool IsDefault(const ThreadLocalPrefs& prefs) {
  return prefs.pinned_at == 0 && prefs.muted_until == 0 && !prefs.archived && !prefs.marked_unread &&
         !prefs.mute_mentions && !prefs.share_ai_confirmed;
}

} // namespace

Roe<std::vector<ThreadLocalPrefs>> InMemoryThreadLocalPrefsStore::List() const {
  std::lock_guard lock(mutex_);
  std::vector<ThreadLocalPrefs> rows;
  for (const auto& [thread_id, prefs] : prefs_) {
    rows.push_back(prefs);
  }
  return rows;
}

Roe<ThreadLocalPrefs> InMemoryThreadLocalPrefsStore::Get(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto prefs = prefs_.find(thread_id);
  if (prefs != prefs_.end()) {
    return prefs->second;
  }
  ThreadLocalPrefs defaults;
  defaults.thread_id = thread_id;
  return defaults;
}

Roe<void> InMemoryThreadLocalPrefsStore::Set(const ThreadLocalPrefs& prefs) const {
  std::lock_guard lock(mutex_);
  if (IsDefault(prefs)) {
    prefs_.erase(prefs.thread_id); // a thread back at the defaults keeps no row
  } else {
    prefs_[prefs.thread_id] = prefs;
  }
  return {};
}

Roe<void> InMemoryThreadLocalPrefsStore::Delete(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  prefs_.erase(thread_id);
  return {};
}

Roe<void> InMemoryThreadMentionsStore::Add(const std::string& thread_id, const std::string& message_id,
                                           int64_t created_at) const {
  std::lock_guard lock(mutex_);
  auto& thread = mentions_[thread_id];
  const bool known = std::any_of(thread.begin(), thread.end(), [&](const auto& m) { return m.second == message_id; });
  if (!known) { // as SQLite's INSERT OR IGNORE: a message is mentioned once
    thread.insert({created_at, message_id});
  }
  return {};
}

Roe<void> InMemoryThreadMentionsStore::DropThread(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  mentions_.erase(thread_id);
  return {};
}

Roe<void> InMemoryThreadMentionsStore::DropMessage(const std::string& thread_id, const std::string& message_id) const {
  std::lock_guard lock(mutex_);
  if (auto thread = mentions_.find(thread_id); thread != mentions_.end()) {
    std::erase_if(thread->second, [&](const auto& m) { return m.second == message_id; });
    if (thread->second.empty()) {
      mentions_.erase(thread);
    }
  }
  return {};
}

Roe<std::set<std::string>> InMemoryThreadMentionsStore::Threads() const {
  std::lock_guard lock(mutex_);
  std::set<std::string> threads;
  for (const auto& [thread_id, mentions] : mentions_) {
    if (!mentions.empty()) {
      threads.insert(thread_id);
    }
  }
  return threads;
}

Roe<std::optional<std::string>> InMemoryThreadMentionsStore::Oldest(const std::string& thread_id) const {
  std::lock_guard lock(mutex_);
  auto thread = mentions_.find(thread_id);
  if (thread == mentions_.end() || thread->second.empty()) {
    return std::optional<std::string>();
  }
  return std::optional<std::string>(thread->second.begin()->second); // by created_at, then message id
}

} // namespace pbr
