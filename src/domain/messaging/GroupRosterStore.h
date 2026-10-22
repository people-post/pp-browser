#pragma once

#include "domain/messaging/GroupTypes.h"

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** Group roster + metadata persistence in profile.db. */
class GroupRosterStore {
public:
  explicit GroupRosterStore(std::string profile_db_path);

  /**
   * Hold across a read-then-write of a group's roster or metadata (an epoch bump, an inbound
   * membership event): those run on several workers at once — two accepts in one inbox poll — and
   * each would bump the epoch from the same value. One lock for every profile: cheap and rare.
   * Recursive: an owner's change holds it across its read and the apply that takes it too.
   */
  [[nodiscard]] static std::unique_lock<std::recursive_mutex> LockForUpdate();

  Roe<void> EnsureSchema(sqlite3* profile_db) const;

  Roe<void> UpsertMetadata(const GroupMetadata& metadata) const;
  Roe<std::optional<GroupMetadata>> LoadMetadata(const std::string& group_id) const;

  Roe<void> UpsertMember(const std::string& group_id, const GroupRosterMember& member) const;
  Roe<void> RemoveMember(const std::string& group_id, const std::string& member_identity) const;
  Roe<std::vector<GroupRosterMember>> ListMembers(const std::string& group_id) const;
  Roe<std::optional<GroupRosterMember>> FindMember(const std::string& group_id,
                                                  const std::string& member_identity) const;
  Roe<bool> IsMember(const std::string& group_id, const std::string& member_identity) const;

  Roe<void> UpsertPendingInvite(const PendingGroupInvite& invite) const;
  Roe<std::optional<PendingGroupInvite>> LoadPendingInvite(const std::string& invite_nonce) const;
  Roe<void> UpdateInviteStatus(const std::string& invite_nonce, InviteStatus status) const;
  Roe<size_t> CountPendingInvitesSince(int64_t since_ms) const;

  Roe<void> UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id,
                              uint32_t session_epoch, uint64_t next_outgoing_seq) const;
  Roe<void> ClearGroupTarget(const std::string& group_id) const;
  Roe<void> ClearGroupTargetByThreadId(const std::string& local_thread_id) const;
  Roe<std::optional<std::string>> FindThreadIdForGroup(const std::string& group_id) const;
  Roe<uint64_t> AllocateGroupSenderSeq(const std::string& group_id) const;
  Roe<uint32_t> GetGroupSessionEpoch(const std::string& group_id) const;
  Roe<void> BumpGroupSessionEpoch(const std::string& group_id, uint32_t new_epoch) const;

private:
  /** profile.db with the busy timeout the other profile stores use (concurrent writers wait). */
  bool OpenDb(sqlite3** db) const;

  std::string profile_db_path_;
};

} // namespace pbr
