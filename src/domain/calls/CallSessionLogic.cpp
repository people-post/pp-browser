#include "domain/calls/CallSessionLogic.h"

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

bool CallSessionLogic::IsGroupCall(const CallSession& session, const size_t offered_participants) {
  return (session.origin_group_id && !session.origin_group_id->empty()) || offered_participants > 2;
}

CallSessionState CallSessionLogic::TransitionOnLeave(const CallSessionState current,
                                                     const size_t remaining_joined, const bool group) {
  if (current == CallSessionState::Ended) {
    return CallSessionState::Ended;
  }
  if (remaining_joined == 0) {
    return CallSessionState::Ended;
  }
  // Active 1:1 (or last-remote leave): do not leave a sole survivor in-call with a dead peer. A group
  // call goes on with whoever is left, alone too (V053).
  if (!group && current == CallSessionState::Active && remaining_joined == 1) {
    return CallSessionState::Ended;
  }
  return current == CallSessionState::Ringing ? CallSessionState::Ringing : CallSessionState::Active;
}

bool CallSessionLogic::CanAcceptJoin(const size_t current_joined_count, const size_t max_joined) {
  return current_joined_count < max_joined;
}

bool CallSessionLogic::VideoAllowedFromInvite(const CallInviteDetail& invite) {
  return invite.video_allowed;
}

bool CallSessionLogic::VoiceAnswerNarrowsCall(const CallSession& session) {
  return !session.origin_group_id || session.origin_group_id->empty();
}

bool CallSessionLogic::ShouldHonorInboundVideoRefresh(const std::string& refresh_call_id,
                                                      const std::string& refresh_publisher_identity,
                                                      const std::string& sender_identity,
                                                      const std::string& local_identity,
                                                      const std::string& active_call_id,
                                                      const bool sender_joined) {
  if (refresh_call_id.empty() || active_call_id.empty() || refresh_call_id != active_call_id) {
    return false;
  }
  if (sender_identity.empty() || !sender_joined) {
    return false;
  }
  if (!refresh_publisher_identity.empty() && refresh_publisher_identity != local_identity) {
    return false;
  }
  return true;
}

bool CallSessionLogic::ShouldAutoLeaveOutboundUnanswered(const bool outbound_calling,
                                                         const bool media_active,
                                                         const int64_t session_created_at_ms,
                                                         const int64_t now_ms, const int64_t ttl_ms) {
  if (!outbound_calling || media_active || session_created_at_ms <= 0 || ttl_ms <= 0) {
    return false;
  }
  return now_ms - session_created_at_ms >= ttl_ms;
}

std::vector<std::string> SelectCallFanoutIdentities(const std::vector<CallParticipant>& participants,
                                                   const std::string& skip_identity, bool include_joined,
                                                   bool include_ringing_or_invited) {
  std::vector<std::string> out;
  out.reserve(participants.size());
  for (const CallParticipant& row : participants) {
    if (row.identity.empty() || row.identity == skip_identity) {
      continue;
    }
    const bool ok_joined = include_joined && row.state == CallParticipantState::Joined;
    const bool ok_ring =
        include_ringing_or_invited &&
        (row.state == CallParticipantState::Ringing || row.state == CallParticipantState::Invited);
    if (!ok_joined && !ok_ring) {
      continue;
    }
    out.push_back(row.identity);
  }
  return out;
}

bool ThreadMatchesActiveCall(const Thread& thread, const CallSession& session,
                             const std::vector<CallParticipant>& joined_participants) {
  if (session.origin_thread_id && *session.origin_thread_id == thread.id) {
    return true;
  }
  if (thread.kind == ThreadKind::Group && thread.group_id && session.origin_group_id &&
      *thread.group_id == *session.origin_group_id) {
    return true;
  }
  if (thread.kind == ThreadKind::Direct && !thread.peer_identity_value.empty()) {
    for (const CallParticipant& row : joined_participants) {
      // Callers typically pass ListJoinedParticipants (Joined-only); ignore other states.
      if (row.identity == thread.peer_identity_value && row.state == CallParticipantState::Joined) {
        return true;
      }
    }
  }
  return false;
}

std::optional<int64_t> CallSessionLogic::RosterJoinStamp(const std::vector<CallRosterEntry>& participants,
                                                         const std::string& identity) {
  for (const CallRosterEntry& entry : participants) {
    if (entry.identity == identity && entry.joined_at) {
      return entry.joined_at;
    }
  }
  return std::nullopt;
}

int64_t CallSessionLogic::LocalJoinStampFallback(const CallSession& session) {
  return session.created_at > 0 ? session.created_at : 1;
}

size_t CallSessionLogic::CountDistinctInvitees(const std::vector<std::string>& invitees,
                                               const std::string& local_identity) {
  std::vector<std::string> seen;
  for (const std::string& identity : invitees) {
    if (!identity.empty() && identity != local_identity &&
        std::find(seen.begin(), seen.end(), identity) == seen.end()) {
      seen.push_back(identity);
    }
  }
  return seen.size();
}

void CallSessionLogic::AppendInvitedCoInvitees(std::vector<CallRosterEntry>& participants,
                                               const std::vector<std::string>& co_invitees,
                                               const std::string& local_identity) {
  for (const std::string& identity : co_invitees) {
    if (identity.empty() || identity == local_identity) {
      continue;
    }
    const bool listed = std::any_of(participants.begin(), participants.end(),
                                    [&identity](const CallRosterEntry& e) { return e.identity == identity; });
    if (!listed) {
      CallRosterEntry entry;
      entry.identity = identity;
      entry.state = CallParticipantState::Invited;
      participants.push_back(std::move(entry));
    }
  }
}

} // namespace pbr
