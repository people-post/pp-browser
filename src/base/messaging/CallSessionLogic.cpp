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
  return current == CallSessionState::Ringing ? CallSessionState::Ringing : CallSessionState::Active;
}

bool CallSessionLogic::CanAcceptJoin(const size_t current_joined_count, const size_t max_joined) {
  return current_joined_count < max_joined;
}

} // namespace pbr
