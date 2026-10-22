#include "domain/calls/CallSessionQueries.h"

#include "domain/calls/SoftMigrateLogic.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace call_queries {

Roe<CallRosterDetail> RosterDetail(const CallSessionStore& sessions, const std::string& call_id) {
  auto session = sessions.LoadSession(call_id);
  if (!session || !session->has_value()) {
    return Error("Call session not found");
  }
  auto participants = sessions.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  CallRosterDetail detail;
  detail.call_id = call_id;
  detail.media_epoch = (*session)->media_epoch;
  for (const CallParticipant& row : *participants) {
    CallRosterEntry entry;
    entry.identity = row.identity;
    entry.state = row.state;
    entry.audio_muted = row.media.audio_muted;
    entry.video_enabled = row.media.video_enabled;
    entry.joined_at = row.joined_at;
    detail.participants.push_back(std::move(entry));
  }
  return detail;
}

Roe<std::optional<CallParticipant>> FirstRemoteParticipant(const CallSessionStore& sessions,
                                                           const std::string& call_id,
                                                           const std::string& local_identity) {
  auto participants = sessions.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& row : *participants) {
    if (row.identity != local_identity && !row.identity.empty()) {
      return std::optional<CallParticipant>{row};
    }
  }
  return std::optional<CallParticipant>{};
}

Roe<std::vector<CallParticipant>> JoinedParticipants(const CallSessionStore& sessions, const std::string& call_id) {
  auto participants = sessions.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  std::vector<CallParticipant> joined;
  joined.reserve(participants->size());
  for (const CallParticipant& row : *participants) {
    if (row.state == CallParticipantState::Joined) {
      joined.push_back(row);
    }
  }
  return joined;
}

std::vector<std::string> JoinedRemoteIdentities(const CallSessionStore& sessions, const std::string& call_id,
                                                const std::string& local_identity) {
  std::vector<std::string> out;
  if (auto joined = JoinedParticipants(sessions, call_id)) {
    for (const CallParticipant& p : *joined) {
      if (!p.identity.empty() && p.identity != local_identity) {
        out.push_back(p.identity);
      }
    }
  }
  return out;
}

Roe<bool> AwaitingExplicitAnswer(const CallSessionStore& sessions, const std::string& call_id) {
  auto participants = sessions.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  for (const CallParticipant& p : *participants) {
    auto invite = sessions.LoadPendingInvite(call_id, p.identity);
    if (invite && invite->has_value() && (*invite)->status == "accepted_implicit") {
      return true;
    }
  }
  return false;
}

bool AwaitsOwnInviteeAccept(const CallSessionStore& sessions, const std::string& call_id,
                            const std::string& local_identity) {
  for (const std::string& identity : JoinedRemoteIdentities(sessions, call_id, local_identity)) {
    auto invite = sessions.LoadPendingInvite(call_id, identity);
    // Inbound CallAccept marks the row "accepted" before the topology sees the join.
    if (invite && invite->has_value() && (*invite)->inviter_identity == local_identity &&
        (*invite)->status == "pending") {
      return true;
    }
  }
  return false;
}

bool IsStickyInitiator(const CallSessionStore& sessions, const std::string& call_id,
                       const std::string& local_identity) {
  auto joined = JoinedParticipants(sessions, call_id);
  if (!joined) {
    return false;
  }
  std::vector<SoftMigrateJoinedPeer> peers;
  peers.reserve(joined->size());
  for (const CallParticipant& p : *joined) {
    SoftMigrateJoinedPeer peer;
    peer.identity = p.identity;
    peer.joined_at = p.joined_at;
    peers.push_back(std::move(peer));
  }
  return SelectCallInitiator(peers) == local_identity;
}

bool SenderIsInCall(const CallSessionStore& sessions, const std::string& call_id,
                    const std::string& sender_identity) {
  if (call_id.empty() || sender_identity.empty()) {
    return false;
  }
  if (auto session = sessions.LoadSession(call_id);
      session && session->has_value() && (*session)->initiator_identity == sender_identity) {
    return true;
  }
  auto participant = sessions.FindParticipant(call_id, sender_identity);
  if (!participant || !participant->has_value()) {
    return false;
  }
  switch ((*participant)->state) {
  case CallParticipantState::Invited:
  case CallParticipantState::Ringing:
  case CallParticipantState::Joined:
    return true;
  case CallParticipantState::Left:
  case CallParticipantState::Declined:
  case CallParticipantState::Missed:
    return false;
  }
  return false;
}

bool SenderMayJoin(const CallSessionStore& sessions, const std::string& call_id, const std::string& sender_identity) {
  if (call_id.empty() || sender_identity.empty()) {
    return false;
  }
  auto session = sessions.LoadSession(call_id);
  if (!session || !session->has_value() || !(*session)->origin_group_id || (*session)->origin_group_id->empty()) {
    return false;
  }
  if ((*session)->initiator_identity == sender_identity) {
    return true;
  }
  auto participant = sessions.FindParticipant(call_id, sender_identity);
  return participant && participant->has_value();
}

std::optional<CallSession> JoinableGroupCall(const CallSessionStore& sessions, const std::string& group_id,
                                             const std::string& local_identity, const int64_t now_ms) {
  if (group_id.empty() || local_identity.empty()) {
    return std::nullopt;
  }
  auto calls = sessions.ListSessionsForGroup(group_id);
  if (!calls) {
    return std::nullopt;
  }
  for (const CallSession& call : *calls) {
    if (call.created_at > 0 && now_ms - call.created_at > kJoinableGroupCallMs) {
      break;  // newest first: the rest are older still
    }
    auto participants = sessions.ListParticipants(call.call_id);
    if (!participants) {
      continue;
    }
    bool offered_and_out = false;
    bool someone_in = false;
    for (const CallParticipant& row : *participants) {
      if (row.identity == local_identity) {
        offered_and_out = row.state == CallParticipantState::Declined || row.state == CallParticipantState::Missed ||
                          row.state == CallParticipantState::Left;
      } else if (row.state == CallParticipantState::Joined) {
        someone_in = true;
      }
    }
    if (offered_and_out && someone_in) {
      return call;
    }
  }
  return std::nullopt;
}

namespace {

bool Unanswered(const CallParticipantState state) {
  return state == CallParticipantState::Invited || state == CallParticipantState::Ringing;
}

bool GroupCall(const CallSession& session) {
  return session.origin_group_id && !session.origin_group_id->empty();
}

} // namespace

std::optional<CallSession> OurUnansweredCallTo(const CallSessionStore& sessions, const std::string& peer,
                                               const std::string& local_identity, const std::string& except_call_id) {
  auto active = sessions.ListActiveSessions();
  if (!active) {
    return std::nullopt;
  }
  for (const CallSession& session : *active) {
    if (session.call_id == except_call_id || session.initiator_identity != local_identity || GroupCall(session)) {
      continue;
    }
    auto participants = sessions.ListParticipants(session.call_id);
    if (!participants) {
      continue;
    }
    bool peer_unanswered = false;
    bool others = false;
    for (const CallParticipant& participant : *participants) {
      if (participant.identity == peer) {
        peer_unanswered = Unanswered(participant.state);
      } else if (participant.identity != local_identity) {
        others = true;
      }
    }
    if (peer_unanswered && !others) {
      return session;
    }
  }
  return std::nullopt;
}

std::optional<CallSession> OurUnansweredGroupCall(const CallSessionStore& sessions, const std::string& group_id,
                                                  const std::string& local_identity, const std::string& except_call_id) {
  auto active = sessions.ListActiveSessions();
  if (!active || group_id.empty()) {
    return std::nullopt;
  }
  for (const CallSession& session : *active) {
    if (session.call_id == except_call_id || session.initiator_identity != local_identity ||
        session.origin_group_id != group_id) {
      continue;
    }
    auto joined = JoinedRemoteIdentities(sessions, session.call_id, local_identity);
    if (joined.empty()) {
      return session;
    }
  }
  return std::nullopt;
}

bool PeerStillToAnswerOurCall(const CallSessionStore& sessions, const std::string& call_id, const std::string& peer,
                              const std::string& local_identity) {
  auto session = sessions.LoadSession(call_id);
  if (!session || !session->has_value() || (*session)->state == CallSessionState::Ended) {
    return false;
  }
  auto participants = sessions.ListParticipants(call_id);
  if (!participants || participants->size() != 2) {
    return false;
  }
  bool local_joined = false;
  bool peer_pending = false;
  for (const CallParticipant& p : *participants) {
    if (p.identity == local_identity) {
      local_joined = p.state == CallParticipantState::Joined;
    } else if (p.identity == peer) {
      peer_pending = Unanswered(p.state);
    }
  }
  return local_joined && peer_pending;
}

} // namespace call_queries

Roe<void> ApplyCallRosterToStore(const CallSessionStore& sessions, const CallRosterDetail& roster,
                                 const std::string& local_identity) {
  if (auto session = sessions.LoadSession(roster.call_id);
      session && session->has_value() && roster.media_epoch > (*session)->media_epoch) {
    (*session)->media_epoch = roster.media_epoch;  // epochs only move forward
    (void)sessions.UpsertSession(**session);
  }
  for (const CallRosterEntry& entry : roster.participants) {
    if (entry.identity.empty()) {
      continue;
    }
    if (!local_identity.empty() && entry.identity == local_identity) {
      if (entry.joined_at) {
        if (auto own = sessions.FindParticipant(roster.call_id, entry.identity);
            own && own->has_value() && (*own)->joined_at != entry.joined_at) {
          CallParticipant stamped = **own;
          stamped.joined_at = entry.joined_at;
          (void)sessions.UpsertParticipant(stamped);
        }
      }
      continue;
    }
    if (entry.state == CallParticipantState::Joined || entry.state == CallParticipantState::Ringing ||
        entry.state == CallParticipantState::Invited) {
      if (auto existing = sessions.FindParticipant(roster.call_id, entry.identity);
          existing && existing->has_value()) {
        const CallParticipantState prior = (*existing)->state;
        // V053: joined again — the one who let them in stamped a join newer than the one we know
        // (who left had joined: a stale roster carries that old stamp). Who declined or missed never
        // joined, so no stale roster lists them joined.
        const bool rejoined =
            entry.state == CallParticipantState::Joined && entry.joined_at &&
            (prior == CallParticipantState::Left ? (*existing)->joined_at && *entry.joined_at > *(*existing)->joined_at
                                                 : true);
        if (!rejoined && (prior == CallParticipantState::Left || prior == CallParticipantState::Declined ||
                          prior == CallParticipantState::Missed)) {
          continue;
        }
        // A joiner's roster still lists earlier invitees as Ringing: a peer that accepted stays
        // Joined (SoftMigrate N≥3 and the in-call UI depend on it); its media follows.
        if (prior == CallParticipantState::Joined &&
            (entry.state == CallParticipantState::Ringing || entry.state == CallParticipantState::Invited)) {
          CallParticipant keep = **existing;
          keep.media.audio_muted = entry.audio_muted;
          keep.media.video_enabled = entry.video_enabled;
          (void)sessions.UpsertParticipant(keep);
          continue;
        }
      }
    }
    CallParticipant participant;
    participant.call_id = roster.call_id;
    participant.identity = entry.identity;
    participant.state = entry.state;
    participant.media.audio_muted = entry.audio_muted;
    participant.media.video_enabled = entry.video_enabled;
    participant.joined_at = entry.joined_at;
    (void)sessions.UpsertParticipant(participant);
  }
  return {};
}

} // namespace pbr
