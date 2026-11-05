#pragma once

// Calls' stores in memory, for its tests and the tests above it (CR024): the call sessions, participants and invites,
// and the media key slots. Never linked into the app or pp-node.
//
// Like the SQLite ones: active sessions are those not ended, newest first; a group's calls newest first; a
// participant keeps the earliest joined_at of a stay and a new stay starts a new one (B102); the invites still to
// answer are the pending ones, newest first; a media key slot is one per (call, epoch), a new key replacing it.

#include "foundation/crypto/IDekConsumer.h"
#include "lib/calls/CallStores.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionStore.h"

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

class InMemoryCallSessionStore : public ICallSessionStore {
public:
  Roe<void> UpsertSession(const CallSession& session) const override;
  Roe<std::optional<CallSession>> LoadSession(const std::string& call_id) const override;
  Roe<std::vector<CallSession>> ListActiveSessions() const override;
  Roe<std::vector<CallSession>> ListSessionsForGroup(const std::string& group_id, size_t limit = 20) const override;
  Roe<std::vector<CallSession>> ListRecentSessions(size_t limit) const override;

  Roe<void> UpsertParticipant(const CallParticipant& participant) const override;
  Roe<std::vector<CallParticipant>> ListParticipants(const std::string& call_id) const override;
  Roe<std::optional<CallParticipant>> FindParticipant(const std::string& call_id,
                                                      const std::string& identity) const override;
  Roe<size_t> CountJoined(const std::string& call_id) const override;

  Roe<void> UpsertPendingInvite(const PendingCallInvite& invite) const override;
  Roe<std::optional<PendingCallInvite>> LoadPendingInvite(const std::string& call_id,
                                                          const std::string& invitee_identity) const override;
  Roe<std::vector<PendingCallInvite>> ListPendingInvitesForInvitee(const std::string& invitee_identity) const override;
  Roe<void> UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                               const std::string& status) const override;

private:
  // The SQLite store's methods are const (each opens its own handle); these are its tables.
  mutable std::mutex mutex_;
  mutable std::map<std::string, CallSession> sessions_;
  /** By (call, identity). */
  mutable std::map<std::pair<std::string, std::string>, CallParticipant> participants_;
  /** By (call, invitee). */
  mutable std::map<std::pair<std::string, std::string>, PendingCallInvite> invites_;
};

class InMemoryCallMediaKeyStore : public ICallMediaKeyStore, public IDekConsumer {
public:
  Roe<std::string> PutEpochKey(const std::string& call_id, uint32_t media_epoch, const ByteVector& key_bytes) override;
  Roe<std::optional<ByteVector>> LoadEpochKey(const std::string& call_id, uint32_t media_epoch) const override;

  // IDekConsumer: it takes the profile's data key as the app's store does, and keeps nothing at rest to use it on.
  Roe<void> SetDek(ByteVector /*dek*/) override { return {}; }
  void ClearDek() override {}

private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, uint32_t>, ByteVector> keys_;
};

/** Both, as a CallStores for `CallStack::InitializeStores`. */
CallStores InMemoryCallStores();

} // namespace pbr
