#include "lib/calls/testing/InMemoryCallStores.h"

#include "common/Utilities.h"

#include <algorithm>

namespace pbr {

Roe<void> InMemoryCallSessionStore::UpsertSession(const CallSession& session) const {
  std::lock_guard lock(mutex_);
  sessions_[session.call_id] = session;
  return {};
}

Roe<std::optional<CallSession>> InMemoryCallSessionStore::LoadSession(const std::string& call_id) const {
  std::lock_guard lock(mutex_);
  auto session = sessions_.find(call_id);
  return session == sessions_.end() ? std::optional<CallSession>() : std::optional<CallSession>(session->second);
}

namespace {

void NewestFirst(std::vector<CallSession>& sessions) {
  std::stable_sort(sessions.begin(), sessions.end(),
                   [](const CallSession& a, const CallSession& b) { return a.created_at > b.created_at; });
}

} // namespace

Roe<std::vector<CallSession>> InMemoryCallSessionStore::ListActiveSessions() const {
  std::lock_guard lock(mutex_);
  std::vector<CallSession> active;
  for (const auto& [id, session] : sessions_) {
    if (session.state != CallSessionState::Ended) {
      active.push_back(session);
    }
  }
  NewestFirst(active);
  return active;
}

Roe<std::vector<CallSession>> InMemoryCallSessionStore::ListSessionsForGroup(const std::string& group_id,
                                                                             size_t limit) const {
  std::lock_guard lock(mutex_);
  std::vector<CallSession> group;
  for (const auto& [id, session] : sessions_) {
    if (session.origin_group_id == group_id) {
      group.push_back(session);
    }
  }
  NewestFirst(group);
  if (group.size() > limit) {
    group.resize(limit);
  }
  return group;
}

Roe<std::vector<CallSession>> InMemoryCallSessionStore::ListRecentSessions(const size_t limit) const {
  std::lock_guard lock(mutex_);
  std::vector<CallSession> all;
  for (const auto& [id, session] : sessions_) {
    all.push_back(session);
  }
  NewestFirst(all);
  if (all.size() > limit) {
    all.resize(limit);
  }
  return all;
}

Roe<void> InMemoryCallSessionStore::UpsertParticipant(const CallParticipant& participant) const {
  std::lock_guard lock(mutex_);
  auto [row, inserted] = participants_.try_emplace({participant.call_id, participant.identity}, participant);
  if (inserted) {
    return {};
  }
  CallParticipant& kept = row->second;
  // The joined_at of a stay is its earliest; coming back after leaving starts a new stay (B102).
  std::optional<int64_t> joined_at;
  const bool rejoin = kept.state == CallParticipantState::Left && (participant.state == CallParticipantState::Joined ||
                                                                   participant.state == CallParticipantState::Invited ||
                                                                   participant.state == CallParticipantState::Ringing);
  if (rejoin || !kept.joined_at) {
    joined_at = participant.joined_at;
  } else if (!participant.joined_at) {
    joined_at = kept.joined_at;
  } else {
    joined_at = std::min(*kept.joined_at, *participant.joined_at);
  }
  kept = participant;
  kept.joined_at = joined_at;
  return {};
}

Roe<std::vector<CallParticipant>> InMemoryCallSessionStore::ListParticipants(const std::string& call_id) const {
  std::lock_guard lock(mutex_);
  std::vector<CallParticipant> out; // by identity, as SQLite's ORDER BY identity
  for (const auto& [key, participant] : participants_) {
    if (key.first == call_id) {
      out.push_back(participant);
    }
  }
  return out;
}

Roe<std::optional<CallParticipant>> InMemoryCallSessionStore::FindParticipant(const std::string& call_id,
                                                                              const std::string& identity) const {
  std::lock_guard lock(mutex_);
  auto participant = participants_.find({call_id, identity});
  return participant == participants_.end() ? std::optional<CallParticipant>()
                                            : std::optional<CallParticipant>(participant->second);
}

Roe<size_t> InMemoryCallSessionStore::CountJoined(const std::string& call_id) const {
  std::lock_guard lock(mutex_);
  return static_cast<size_t>(std::count_if(participants_.begin(), participants_.end(), [&](const auto& entry) {
    return entry.first.first == call_id && entry.second.state == CallParticipantState::Joined;
  }));
}

Roe<void> InMemoryCallSessionStore::UpsertPendingInvite(const PendingCallInvite& invite) const {
  std::lock_guard lock(mutex_);
  invites_[{invite.call_id, invite.invitee_identity}] = invite;
  return {};
}

Roe<std::optional<PendingCallInvite>> InMemoryCallSessionStore::LoadPendingInvite(
    const std::string& call_id, const std::string& invitee_identity) const {
  std::lock_guard lock(mutex_);
  auto invite = invites_.find({call_id, invitee_identity});
  return invite == invites_.end() ? std::optional<PendingCallInvite>()
                                  : std::optional<PendingCallInvite>(invite->second);
}

Roe<std::vector<PendingCallInvite>> InMemoryCallSessionStore::ListPendingInvitesForInvitee(
    const std::string& invitee_identity) const {
  std::lock_guard lock(mutex_);
  std::vector<PendingCallInvite> pending;
  for (const auto& [key, invite] : invites_) {
    if (key.second == invitee_identity && invite.status == "pending") {
      pending.push_back(invite);
    }
  }
  std::stable_sort(pending.begin(), pending.end(),
                   [](const PendingCallInvite& a, const PendingCallInvite& b) { return a.created_at > b.created_at; });
  return pending;
}

Roe<void> InMemoryCallSessionStore::UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                                                       const std::string& status) const {
  std::lock_guard lock(mutex_);
  if (auto invite = invites_.find({call_id, invitee_identity}); invite != invites_.end()) {
    invite->second.status = status;
  }
  return {};
}

Roe<std::string> InMemoryCallMediaKeyStore::PutEpochKey(const std::string& call_id, uint32_t media_epoch,
                                                        const ByteVector& key_bytes) {
  if (call_id.empty() || key_bytes.empty()) {
    return Error("call_id and key_bytes required");
  }
  std::lock_guard lock(mutex_);
  keys_[{call_id, media_epoch}] = key_bytes;
  return std::string("mk:") + util::GenerateUuid();
}

Roe<std::optional<ByteVector>> InMemoryCallMediaKeyStore::LoadEpochKey(const std::string& call_id,
                                                                       uint32_t media_epoch) const {
  std::lock_guard lock(mutex_);
  auto key = keys_.find({call_id, media_epoch});
  return key == keys_.end() ? std::optional<ByteVector>() : std::optional<ByteVector>(key->second);
}

CallStores InMemoryCallStores() {
  CallStores stores;
  auto media_keys = std::make_unique<InMemoryCallMediaKeyStore>();
  stores.at_rest = {media_keys.get()};
  stores.media_keys = std::move(media_keys);
  stores.sessions = std::make_unique<InMemoryCallSessionStore>();
  return stores;
}

} // namespace pbr
