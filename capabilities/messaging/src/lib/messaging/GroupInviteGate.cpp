#include "lib/messaging/GroupInviteGate.h"

#include "common/Utilities.h"

#include <algorithm>

#include "common/PbrCompat.h"

namespace pbr {

GroupInviteGate::GroupInviteGate(StandingFn standing, IGroupRosterStore& roster)
    : standing_(std::move(standing)), roster_(roster) {}

void GroupInviteGate::SetInboundPolicy(const GroupInvitePolicy policy) {
  inbound_policy_ = policy;
}

void GroupInviteGate::SetInLiveCall(InLiveCallFn in_live_call) {
  in_live_call_ = std::move(in_live_call);
}

Roe<bool> GroupInviteGate::AllowsInboundInvite(const GroupInvitePayload& invite) const {
  if (inbound_policy_ == GroupInvitePolicy::Nobody) {
    return false;
  }
  const InviterStanding standing = standing_ ? standing_(invite.inviter_identity) : InviterStanding::Unknown;
  if (standing == InviterStanding::Blocked) {
    return false;
  }
  if (standing == InviterStanding::Unknown && inbound_policy_ == GroupInvitePolicy::ContactsOnly) {
    // G022: someone met in the call we are both in right now is not a stranger for the invite they made there. It
    // stays the ordinary invite: nobody is added without Join.
    const bool met_in_call =
        invite.MadeInCall() && in_live_call_ && in_live_call_(invite.call_id, invite.inviter_identity);
    if (!met_in_call) {
      return false;
    }
  }
  if (invite.expires_at && *invite.expires_at < util::NowUnixMs()) {
    return false;
  }
  const int64_t since = util::NowUnixMs() - 24LL * 60 * 60 * 1000;
  auto pending_count = roster_.CountPendingInvitesSince(since);
  if (!pending_count) {
    return pending_count.error();
  }
  if (*pending_count >= kMaxPendingInvitesPerDay) {
    return false;
  }
  return true;
}

Roe<void> GroupInviteGate::RecordPendingInvite(const PendingGroupInvite& invite) const {
  return roster_.UpsertPendingInvite(invite);
}

void GroupInviteGate::NoteInviteInCall(const GroupInvitePayload& invite) {
  if (!invite.MadeInCall() || invite.invite_nonce.empty()) {
    return;
  }
  std::lock_guard lock(in_call_mutex_);
  if (in_call_.count(invite.invite_nonce) == 0) {
    in_call_order_.push_back(invite.invite_nonce);
  }
  in_call_[invite.invite_nonce] = InviteInCall{.invite_nonce = invite.invite_nonce,
                                               .group_id = invite.group_id,
                                               .group_title = invite.group_title,
                                               .inviter_identity = invite.inviter_identity,
                                               .call_id = invite.call_id,
                                               .expires_at = invite.expires_at};
}

void GroupInviteGate::ForgetInviteInCall(const std::string& invite_nonce) {
  std::lock_guard lock(in_call_mutex_);
  if (in_call_.erase(invite_nonce) > 0) {
    in_call_order_.erase(std::remove(in_call_order_.begin(), in_call_order_.end(), invite_nonce), in_call_order_.end());
  }
}

std::vector<GroupInviteGate::InviteInCall> GroupInviteGate::InvitesInCall(const std::string& call_id,
                                                                          const int64_t now_ms) const {
  std::vector<InviteInCall> out;
  if (call_id.empty()) {
    return out;
  }
  std::lock_guard lock(in_call_mutex_);
  for (const std::string& nonce : in_call_order_) {
    const auto it = in_call_.find(nonce);
    if (it == in_call_.end() || it->second.call_id != call_id) {
      continue;
    }
    if (it->second.expires_at && *it->second.expires_at < now_ms) {
      continue;
    }
    out.push_back(it->second);
  }
  return out;
}

std::string GroupInviteGate::CallOfInvite(const std::string& invite_nonce) const {
  std::lock_guard lock(in_call_mutex_);
  const auto it = in_call_.find(invite_nonce);
  return it == in_call_.end() ? std::string() : it->second.call_id;
}

} // namespace pbr
