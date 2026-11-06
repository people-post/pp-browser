#pragma once

// Messaging's stores in memory, for its tests and the tests of capabilities above it (CR024): the group rosters,
// the per-thread settings and the unread mentions. Never linked into the app or pp-node.
//
// The roster shares each group's send target (its thread and numbering) with delivery's in-memory store, as the
// SQLite roster and thread store share `group_targets`. Like the SQLite ones: an invite once stored is not
// overwritten; ending a group voids its pending invites, empties its roster, drops its target and records the
// sends, in one step; a member's pair numbering restarts at 1 in a newer pair epoch.

#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/IThreadLocalPrefsStore.h"
#include "lib/messaging/IThreadMentionsStore.h"

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class InMemoryGroupRosterStore : public IGroupRosterStore {
public:
  /**
   * `threads` keeps the groups' send targets (and must outlive this); it takes messaging's rule for group events in
   * a direct chat (they do not move it), as the app's storage does.
   */
  explicit InMemoryGroupRosterStore(InMemoryDeliveryStore& threads);

  void SetOnChanged(std::function<void(const GroupRosterChange& change)> on_changed) override;

  Roe<void> UpsertMetadata(const GroupMetadata& metadata) const override;
  Roe<std::optional<GroupMetadata>> LoadMetadata(const std::string& group_id) const override;
  Roe<std::vector<std::string>> ListGroupIds() const override;

  Roe<void> UpsertMember(const std::string& group_id, const GroupRosterMember& member) const override;
  Roe<void> RemoveMember(const std::string& group_id, const std::string& member_identity) const override;
  Roe<std::vector<GroupRosterMember>> ListMembers(const std::string& group_id) const override;
  Roe<std::optional<GroupRosterMember>> FindMember(const std::string& group_id,
                                                   const std::string& member_identity) const override;
  Roe<bool> IsMember(const std::string& group_id, const std::string& member_identity) const override;

  Roe<void> UpsertPendingInvite(const PendingGroupInvite& invite) const override;
  Roe<std::optional<PendingGroupInvite>> LoadPendingInvite(const std::string& invite_nonce) const override;
  Roe<void> UpdateInviteStatus(const std::string& invite_nonce, InviteStatus status) const override;
  Roe<size_t> CountPendingInvitesSince(int64_t since_ms) const override;
  Roe<void> ExpireOutboundInvitesTo(const std::string& group_id, const std::string& invitee_identity) const override;
  Roe<std::vector<PendingGroupInvite>> ExpirePendingInvitesOf(const std::string& group_id) const override;
  Roe<std::optional<std::vector<PendingGroupInvite>>> EndGroup(const std::string& group_id,
                                                               const GroupEnding& ending) const override;
  Roe<std::vector<PendingGroupSend>> ListPendingSends(const std::string& group_id = {}) const override;
  Roe<void> DeletePendingSend(const PendingGroupSend& send) const override;
  Roe<std::vector<PendingGroupInvite>> ListOutboundPendingInvites(const std::string& group_id,
                                                                  int64_t now_ms) const override;

  Roe<void> UpsertGroupChange(const StoredGroupChange& change) const override;
  Roe<std::vector<StoredGroupChange>> ListGroupChanges(const std::string& group_id) const override;
  Roe<void> DeleteGroupChanges(const std::string& group_id, const std::vector<std::string>& change_ids) const override;
  Roe<std::vector<std::string>> ListGroupsWithChanges() const override;

  Roe<void> UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id, uint32_t session_epoch,
                              uint64_t next_outgoing_seq) const override;
  Roe<void> ClearGroupTarget(const std::string& group_id) const override;
  Roe<void> ClearGroupTargetByThreadId(const std::string& local_thread_id) const override;
  Roe<std::optional<std::string>> FindThreadIdForGroup(const std::string& group_id) const override;
  Roe<uint64_t> AllocateGroupSenderSeq(const std::string& group_id) const override;
  Roe<uint64_t> AllocateMemberSeq(const std::string& group_id, const std::string& member_identity, uint32_t pair_epoch,
                                  const std::string& member_device = {}) const override;
  Roe<uint32_t> GetGroupSessionEpoch(const std::string& group_id) const override;
  Roe<void> BumpGroupSessionEpoch(const std::string& group_id, uint32_t new_epoch) const override;

private:
  struct MemberSeq {
    uint32_t session_epoch = 0;
    uint64_t next_seq = 1;
  };

  std::vector<PendingGroupInvite> VoidPendingInvitesLocked(const std::string& group_id) const;
  void NotifyChanged(GroupRosterChange change) const;

  InMemoryDeliveryStore& threads_;
  std::function<void(const GroupRosterChange&)> on_changed_;
  // The SQLite store's methods are const (each opens its own handle); these are its tables.
  mutable std::mutex mutex_;
  mutable std::map<std::string, GroupMetadata> metadata_;
  /** Per group, by member identity. */
  mutable std::map<std::string, std::map<std::string, GroupRosterMember>> rosters_;
  mutable std::map<std::string, PendingGroupInvite> invites_;
  /** By (group, recipient, control type). */
  mutable std::map<std::tuple<std::string, std::string, std::string>, PendingGroupSend> sends_;
  /** By (group, member identity or identity/device). */
  mutable std::map<std::pair<std::string, std::string>, MemberSeq> member_seqs_;
  /** Per group, by change id. */
  mutable std::map<std::string, std::map<std::string, StoredGroupChange>> changes_;
};

class InMemoryThreadLocalPrefsStore : public IThreadLocalPrefsStore {
public:
  Roe<std::vector<ThreadLocalPrefs>> List() const override;
  Roe<ThreadLocalPrefs> Get(const std::string& thread_id) const override;
  Roe<void> Set(const ThreadLocalPrefs& prefs) const override;
  Roe<void> Delete(const std::string& thread_id) const override;

private:
  mutable std::mutex mutex_;
  mutable std::map<std::string, ThreadLocalPrefs> prefs_;
};

class InMemoryThreadMentionsStore : public IThreadMentionsStore {
public:
  Roe<void> Add(const std::string& thread_id, const std::string& message_id, int64_t created_at) const override;
  Roe<void> MarkThreadRead(const std::string& thread_id) const override;
  Roe<void> DropThread(const std::string& thread_id) const override;
  Roe<void> DropMessage(const std::string& thread_id, const std::string& message_id) const override;
  Roe<std::set<std::string>> Threads() const override;
  Roe<std::map<std::string, int>> Counts() const override;
  Roe<std::optional<std::string>> Oldest(const std::string& thread_id) const override;
  Roe<std::optional<Mention>> Latest() const override;

private:
  mutable std::mutex mutex_;
  /** Per thread: (created_at, message id), oldest first. */
  mutable std::map<std::string, std::set<std::pair<int64_t, std::string>>> mentions_;
  /** Per thread: its newest read mention (as SQLite's thread_mentions_read). */
  mutable std::map<std::string, std::pair<int64_t, std::string>> read_;
};

} // namespace pbr
