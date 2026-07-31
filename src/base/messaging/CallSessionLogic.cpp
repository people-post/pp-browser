#include "base/messaging/CallSessionLogic.h"

#include <algorithm>

namespace pbr {

std::optional<std::string> CallSessionLogic::SelectEpochCoordinator(
    const std::vector<std::string>& joined_identities) {
  std::optional<std::string> best;
  for (const std::string& identity : joined_identities) {
    if (identity.empty()) {
      continue;
    }
    if (!best || identity < *best) {
      best = identity;
    }
  }
  return best;
}

bool CallSessionLogic::IsInviteExpired(const PendingCallInvite& invite, const int64_t now_ms) {
  if (!invite.expires_at) {
    return false;
  }
  return now_ms >= *invite.expires_at;
}

bool CallSessionLogic::IsInviteExpired(const CallInviteDetail& invite, const int64_t now_ms) {
  if (!invite.expires_at) {
    return false;
  }
  return now_ms >= *invite.expires_at;
}

int64_t CallSessionLogic::RelayInviteAgeMs(const int64_t relay_created_at_ms,
                                           const int64_t relay_server_time_ms) {
  return relay_server_time_ms - relay_created_at_ms;
}

int64_t CallSessionLogic::DeltaRelayReceiverMs(const int64_t recv_local_ms,
                                               const int64_t relay_server_time_ms) {
  return recv_local_ms - relay_server_time_ms;
}

bool CallSessionLogic::ShouldDropStaleInvite(const CallInviteDetail& invite, const int64_t now_ms,
                                             const std::optional<int64_t> relay_created_at_ms,
                                             const std::optional<int64_t> relay_server_time_ms) {
  if (relay_created_at_ms && relay_server_time_ms && *relay_created_at_ms > 0 &&
      *relay_server_time_ms > 0) {
    const int64_t age = RelayInviteAgeMs(*relay_created_at_ms, *relay_server_time_ms);
    return age > kDefaultCallInviteTtlMs + kCallInviteRelayAgeSlackMs;
  }
  if (!invite.expires_at) {
    return false;
  }
  // Wire expiry past local now by more than skew slack → stale backlog, do not re-arm.
  return now_ms > *invite.expires_at + kCallInviteWireSkewSlackMs;
}

std::vector<PendingCallInvite> CallSessionLogic::ExpirePendingInvites(std::vector<PendingCallInvite> invites,
                                                                      const int64_t now_ms) {
  for (PendingCallInvite& invite : invites) {
    if (invite.status != "pending") {
      continue;
    }
    if (IsInviteExpired(invite, now_ms)) {
      invite.status = "expired";
    }
  }
  return invites;
}

CallSessionState CallSessionLogic::TransitionOnRemoteJoined(const CallSessionState current) {
  if (current == CallSessionState::Ended) {
    return CallSessionState::Ended;
  }
  return CallSessionState::Active;
}

CallSessionState CallSessionLogic::TransitionOnLeave(const CallSessionState current,
                                                     const size_t remaining_joined) {
  if (current == CallSessionState::Ended) {
    return CallSessionState::Ended;
  }
  if (remaining_joined == 0) {
    return CallSessionState::Ended;
  }
  // Active 1:1 (or last-remote leave): do not leave a sole survivor in-call with a dead peer.
  if (current == CallSessionState::Active && remaining_joined == 1) {
    return CallSessionState::Ended;
  }
  return current == CallSessionState::Ringing ? CallSessionState::Ringing : CallSessionState::Active;
}

bool CallSessionLogic::CanAcceptJoin(const size_t current_joined_count, const size_t max_joined) {
  return current_joined_count < max_joined;
}

} // namespace pbr
