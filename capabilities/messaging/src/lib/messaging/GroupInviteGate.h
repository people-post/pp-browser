#pragma once

#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/GroupTypes.h"

#include "common/Error.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * G007 — inbound group invite spam controls; G022 — the consent an invite made during a call carries, and the
 * invites that came in a call this device is in (the call shows them as a banner).
 */
class GroupInviteGate {
public:
  static constexpr size_t kMaxPendingInvitesPerDay = 20;

  /** How this device stands with an inviter: a contact, blocked, or neither (people's to say). */
  enum class InviterStanding { Unknown, Contact, Blocked };
  using StandingFn = std::function<InviterStanding(const std::string& identity)>;
  /**
   * G022: this device is in the live call `call_id` right now, and `inviter_identity` is in it too. The calls' to
   * say — filled in the product layer (messaging never names calls). Any thread.
   */
  using InLiveCallFn = std::function<bool(const std::string& call_id, const std::string& inviter_identity)>;

  /** An invite to a group made during a call, still waiting here (G022). */
  struct InviteInCall {
    std::string invite_nonce;
    std::string group_id;
    std::string group_title;
    std::string inviter_identity;
    std::string call_id;
    std::optional<int64_t> expires_at;
  };

  GroupInviteGate(StandingFn standing, IGroupRosterStore& roster);

  void SetInboundPolicy(GroupInvitePolicy policy);
  GroupInvitePolicy inbound_policy() const { return inbound_policy_; }
  /** G022: how the gate asks about live calls (empty: no call makes a stranger's invite welcome). */
  void SetInLiveCall(InLiveCallFn in_live_call);

  /**
   * Whether an inbound invite may be kept: never from a blocked inviter, nor under `nobody`; under `contacts_only` a
   * stranger's only when it was made during a call this device is in with them right now (G022 amendment).
   */
  Roe<bool> AllowsInboundInvite(const GroupInvitePayload& invite) const;
  Roe<void> RecordPendingInvite(const PendingGroupInvite& invite) const;

  /** G022: an inbound invite made during a call was kept; the call shows it until answered. No-op for other invites. */
  void NoteInviteInCall(const GroupInvitePayload& invite);
  /** The invite was answered (accepted, declined): it leaves the call's banner. */
  void ForgetInviteInCall(const std::string& invite_nonce);
  /** The invites waiting in the call `call_id`, oldest first, the expired ones left out. */
  std::vector<InviteInCall> InvitesInCall(const std::string& call_id, int64_t now_ms) const;
  /** The call an invite came in (empty: none, or not since this device started). */
  std::string CallOfInvite(const std::string& invite_nonce) const;

private:
  StandingFn standing_;
  IGroupRosterStore& roster_;
  GroupInvitePolicy inbound_policy_ = GroupInvitePolicy::ContactsOnly;
  InLiveCallFn in_live_call_;
  /** In memory: a call does not outlive the app, and the invite card in the DM stays either way. */
  mutable std::mutex in_call_mutex_;
  std::map<std::string, InviteInCall> in_call_; // by nonce
  std::vector<std::string> in_call_order_;
};

} // namespace pbr
