#pragma once

#include <functional>

#include "lib/messaging/IGroupRosterStore.h"

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** IGroupRosterStore on profile.db. */
class SqliteGroupRosterStore : public IGroupRosterStore {
public:
  explicit SqliteGroupRosterStore(std::string profile_db_path);

  Roe<void> EnsureSchema(sqlite3* profile_db) const;

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
  Roe<std::vector<PendingGroupInvite>> ListOutboundPendingInvites(const std::string& group_id, int64_t now_ms) const override;

  Roe<void> UpsertGroupChange(const StoredGroupChange& change) const override;
  Roe<std::vector<StoredGroupChange>> ListGroupChanges(const std::string& group_id) const override;
  Roe<void> DeleteGroupChanges(const std::string& group_id, const std::vector<std::string>& change_ids) const override;
  Roe<std::vector<std::string>> ListGroupsWithChanges() const override;

  Roe<void> UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id,
                              uint32_t session_epoch, uint64_t next_outgoing_seq) const override;
  Roe<void> ClearGroupTarget(const std::string& group_id) const override;
  Roe<void> ClearGroupTargetByThreadId(const std::string& local_thread_id) const override;
  Roe<std::optional<std::string>> FindThreadIdForGroup(const std::string& group_id) const override;
  Roe<uint64_t> AllocateGroupSenderSeq(const std::string& group_id) const override;
  Roe<uint64_t> AllocateMemberSeq(const std::string& group_id, const std::string& member_identity,
                                  uint32_t pair_epoch, const std::string& member_device = {}) const override;
  Roe<uint32_t> GetGroupSessionEpoch(const std::string& group_id) const override;
  Roe<void> BumpGroupSessionEpoch(const std::string& group_id, uint32_t new_epoch) const override;

private:
  /** profile.db with the busy timeout the other profile stores use (concurrent writers wait). */
  bool OpenDb(sqlite3** db) const;

  void NotifyChanged(GroupRosterChange change) const;

  std::string profile_db_path_;
  std::function<void(const GroupRosterChange&)> on_changed_;
};

} // namespace pbr
