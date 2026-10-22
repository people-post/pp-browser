#pragma once

#include "domain/calls/CallTypes.h"

#include "common/Error.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** Call session + participant + pending invite persistence in profile.db (V011). */
class CallSessionStore {
public:
  explicit CallSessionStore(std::string profile_db_path);

  /** Create or extend the call tables in `profile_db` (idempotent; calls own them). */
  static Roe<void> EnsureSchema(sqlite3* profile_db);

  Roe<void> UpsertSession(const CallSession& session) const;
  Roe<std::optional<CallSession>> LoadSession(const std::string& call_id) const;
  Roe<std::vector<CallSession>> ListActiveSessions() const;
  /** A group's calls in any state, newest first (at most `limit`). */
  Roe<std::vector<CallSession>> ListSessionsForGroup(const std::string& group_id, size_t limit = 20) const;

  Roe<void> UpsertParticipant(const CallParticipant& participant) const;
  Roe<std::vector<CallParticipant>> ListParticipants(const std::string& call_id) const;
  Roe<std::optional<CallParticipant>> FindParticipant(const std::string& call_id,
                                                      const std::string& identity) const;
  Roe<size_t> CountJoined(const std::string& call_id) const;

  Roe<void> UpsertPendingInvite(const PendingCallInvite& invite) const;
  Roe<std::optional<PendingCallInvite>> LoadPendingInvite(const std::string& call_id,
                                                          const std::string& invitee_identity) const;
  Roe<std::vector<PendingCallInvite>> ListPendingInvitesForInvitee(const std::string& invitee_identity) const;
  Roe<void> UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                               const std::string& status) const;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
  /** The schema runs once per store, not on every short-lived connection. */
  mutable std::mutex schema_mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
