#include "domain/messaging/CallThreadPresenceLogic.h"

#include "common/thread/ThreadChannel.h"

namespace pbr {

bool IsCallControlShadowThread(const Thread& thread, const std::vector<Thread>& all_threads) {
  if (thread.kind != ThreadKind::Direct || thread.channel != ThreadChannel::E2ePublic) {
    return false;
  }
  if (!thread.preview.empty() || thread.unread_count != 0) {
    return false;
  }
  if (thread.peer_identity_value.empty()) {
    return false;
  }
  for (const Thread& other : all_threads) {
    if (other.id == thread.id || other.kind != ThreadKind::Direct) {
      continue;
    }
    if (other.peer_identity_value != thread.peer_identity_value) {
      continue;
    }
    if (other.channel == ThreadChannel::E2e) {
      return true;
    }
  }
  return false;
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

} // namespace pbr
