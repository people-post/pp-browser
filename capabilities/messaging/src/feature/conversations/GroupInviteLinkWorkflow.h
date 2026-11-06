#pragma once

#include "feature/conversations/GroupMembershipWorkflow.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "lib/messaging/GroupInviteLink.h"
#include "lib/messaging/GroupLinkJoin.h"
#include "lib/messaging/IGroupInviteLinkStore.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"
#include "common/Module.h"
#include "common/thread/ThreadRecordTypes.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** A live invite link of a group, as group info lists it (G021). */
struct GroupLinkRow {
  std::string link_id;
  std::string maker;
  int64_t expires_at = 0;
  uint64_t max_uses = 0;
  uint64_t uses = 0;
  /** Its text (`pp://join/…`) when this device made it and keeps its secret; else empty. */
  std::string text;
  /** We may revoke it: its maker, or the owner or an admin. */
  bool can_revoke = false;
};

/** Where a join by link of ours stands (the joiner's side). */
struct GroupJoinState {
  enum class Phase : uint8_t {
    /** The request is out; no answer yet. */
    Asking,
    /** The maker's device previewed the group: Join / Cancel. */
    Preview,
    Refused,
    /** No answer within kAnswerTimeoutMs: the maker's device may be offline (the link names nobody else). */
    NoAnswer,
    /** Confirmed: we are in the group's chat, waiting for the join bundle. */
    Joined,
    Cancelled,
  };
  std::string request_id;
  std::string maker;
  Phase phase = Phase::Asking;
  std::optional<GroupJoinPreview> preview;
  GroupJoinRefusal reason = GroupJoinRefusal::Unknown;
  std::string thread_id;
};

/**
 * Invite links (G021), all three sides on one device: the maker makes and revokes links (signed `link_add` /
 * `link_revoke`, the secret kept encrypted to show the link again); the maker's device answers join requests on the
 * control lane (GroupLinkJoinFamily) by the roster it holds and admits a confirmed joiner (GroupMembershipWorkflow's
 * add, with the link's id); and the joiner asks, sees the preview and confirms. Join state lives in memory: a join
 * outlives no restart (the joiner asks again).
 */
class GroupInviteLinkWorkflow : public Module {
public:
  GroupInviteLinkWorkflow(IGroupRosterStore& roster, IGroupInviteLinkStore* links, GroupMembershipWorkflow& groups,
                          MeshDeliveryOrchestrator& mesh, IdentityStore& identity);
  ~GroupInviteLinkWorkflow();

  // ---- The maker ----

  struct MadeLink {
    std::string link_id;
    std::string text;
    int64_t expires_at = 0;
    uint64_t max_uses = 0;
  };
  /**
   * A new link to the group (`GroupAllows(MakeInviteLink…)`): a random id and secret, our signed `link_add` to the
   * members, the secret kept here. `lifetime_ms` 0: the default 7 days; `max_uses` 0: no limit.
   */
  Roe<MadeLink> MakeLink(const std::string& group_id, uint64_t max_uses = 0, int64_t lifetime_ms = 0);
  /** Our `link_revoke` (the link's maker, an admin or the owner). */
  Roe<void> RevokeLink(const std::string& group_id, const std::string& link_id);
  /** The group's live links, by expiry (soonest first); each with its text when it is ours. */
  std::vector<GroupLinkRow> LiveLinks(const std::string& group_id);
  /** Our newest live link to the group whose text we keep (the "Invite link…" sheet shows it before making one). */
  std::optional<GroupLinkRow> OwnLiveLink(const std::string& group_id);

  // ---- The answerer (from the control lane, off the ingest) ----

  void OnRequest(const GroupJoinRequest& request, const std::string& joiner);
  void OnConfirm(const GroupJoinConfirm& confirm, const std::string& joiner);

  // ---- The joiner ----

  /**
   * Ask to join by `link_text` (pasted, scanned or opened): the request to the link's maker. The request id; its state
   * moves on as the answer comes (JoinState, the on-change hook). A link that expired already is refused here.
   */
  Roe<std::string> AskToJoin(const std::string& link_text);
  void OnPreview(const GroupJoinPreview& preview, const std::string& answerer);
  void OnRefused(const GroupJoinRefused& refused, const std::string& answerer);
  /** The preview's Join: the group's chat here, and the confirm to the device that previewed it. */
  Roe<Thread> ConfirmJoin(const std::string& request_id);
  void CancelJoin(const std::string& request_id);
  std::optional<GroupJoinState> JoinState(const std::string& request_id) const;
  /** Called (on a worker) each time a join of ours moves on. */
  void SetOnJoinChanged(std::function<void(GroupJoinState state)> on_changed);

private:
  struct Answering {
    std::string joiner;
    std::string group_id;
    std::string link_id;
    ByteVector secret;
    int64_t at = 0;
  };
  Roe<std::string> LocalAccount() const;
  /** The join case for `link_id` here: the group that knows it (or the one our kept link names) and its roster. */
  std::optional<GroupJoinRefusal> Judge(const std::string& link_id, const ByteVector& secret, const std::string& joiner,
                                        bool count_rate, std::string* group_id, std::optional<GroupRosterView>* view);
  void Send(const std::string& peer, const GroupJoinMessage& message);
  void Refuse(const std::string& joiner, const std::string& request_id, GroupJoinRefusal reason);
  /** Set the state of a join of ours (under `mutex_`'s caller) and tell the hook (outside the lock). */
  void Moved(const GroupJoinState& state);
  void ScheduleNoAnswer(const std::string& request_id);
  void DropStale(int64_t now_ms);

  IGroupRosterStore& roster_;
  IGroupInviteLinkStore* links_;
  GroupMembershipWorkflow& groups_;
  MeshDeliveryOrchestrator& mesh_;
  IdentityStore& identity_;
  group_link_join::JoinRequestLimiter limiter_;

  mutable std::mutex mutex_;
  /** Requests we answered with a preview, by request id (the answerer's side). */
  std::map<std::string, Answering> answering_;
  /** Our joins, by request id, with when they were asked (the joiner's side). */
  std::map<std::string, std::pair<GroupJoinState, int64_t>> joins_;
  std::function<void(GroupJoinState)> on_changed_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pbr
