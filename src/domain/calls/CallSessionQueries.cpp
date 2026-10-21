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

} // namespace call_queries

Roe<void> ApplyCallRosterToStore(const CallSessionStore& sessions, const CallRosterDetail& roster,
                                 const std::string& local_identity) {
  if (auto session = sessions.LoadSession(roster.call_id); session && session->has_value()) {
    (*session)->media_epoch = roster.media_epoch;
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
        if (prior == CallParticipantState::Left || prior == CallParticipantState::Declined ||
            prior == CallParticipantState::Missed) {
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
