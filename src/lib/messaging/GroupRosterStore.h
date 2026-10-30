#pragma once

#include <functional>

#include "lib/messaging/GroupTypes.h"

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

  /** Called after any change to a group's metadata or roster (the account backup is rewritten on it). */
  void SetOnChanged(std::function<void(const std::string& group_id)> on_changed);

  Roe<void> UpsertMetadata(const GroupMetadata& metadata) const;
  Roe<std::optional<GroupMetadata>> LoadMetadata(const std::string& group_id) const;
  /** Every group this profile holds (the account backup carries them). */
  Roe<std::vector<std::string>> ListGroupIds() const;

  Roe<void> UpsertMember(const std::string& group_id, const GroupRosterMember& member) const;
  Roe<void> RemoveMember(const std::string& group_id, const std::string& member_identity) const;
  Roe<std::vector<GroupRosterMember>> ListMembers(const std::string& group_id) const;
  Roe<std::optional<GroupRosterMember>> FindMember(const std::string& group_id,
                                                  const std::string& member_identity) const;
  Roe<bool> IsMember(const std::string& group_id, const std::string& member_identity) const;

  Roe<void> UpsertPendingInvite(const PendingGroupInvite& invite) const;
  Roe<std::optional<PendingGroupInvite>> LoadPendingInvite(const std::string& invite_nonce) const;
  Roe<void> UpdateInviteStatus(const std::string& invite_nonce, InviteStatus status) const;
  /** Inbound pending invites only: the ones we sent do not count against the received cap. */
  Roe<size_t> CountPendingInvitesSince(int64_t since_ms) const;
  /** Marks this group's live outbound invites to one identity expired (it was removed). */
  Roe<void> ExpireOutboundInvitesTo(const std::string& group_id, const std::string& invitee_identity) const;
  /**
   * The group's invites this device sent that are still Pending and not expired at `now_ms`: the latest
   * one per invitee, newest first.
   */
  Roe<std::vector<PendingGroupInvite>> ListOutboundPendingInvites(const std::string& group_id, int64_t now_ms) const;

  Roe<void> UpsertGroupTarget(const std::string& group_id, const std::string& local_thread_id,
                              uint32_t session_epoch, uint64_t next_outgoing_seq) const;
  Roe<void> ClearGroupTarget(const std::string& group_id) const;
  Roe<void> ClearGroupTargetByThreadId(const std::string& local_thread_id) const;
  Roe<std::optional<std::string>> FindThreadIdForGroup(const std::string& group_id) const;
  Roe<uint64_t> AllocateGroupSenderSeq(const std::string& group_id) const;
  /**
   * Our next seq for `member_identity`'s copy of a group message, at the pair's session epoch (G010/G011) —
   * the pair with `member_device`, one of the member's devices (M021), or with none the member's account.
   */
  Roe<uint64_t> AllocateMemberSeq(const std::string& group_id, const std::string& member_identity,
                                  uint32_t pair_epoch, const std::string& member_device = {}) const;
  Roe<uint32_t> GetGroupSessionEpoch(const std::string& group_id) const;
  Roe<void> BumpGroupSessionEpoch(const std::string& group_id, uint32_t new_epoch) const;

private:
  /** profile.db with the busy timeout the other profile stores use (concurrent writers wait). */
  bool OpenDb(sqlite3** db) const;

  void NotifyChanged(const std::string& group_id) const;

  std::string profile_db_path_;
  std::function<void(const std::string& group_id)> on_changed_;
};

} // namespace pbr
