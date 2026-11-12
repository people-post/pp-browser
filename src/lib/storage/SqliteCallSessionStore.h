#pragma once

#include "lib/calls/ICallSessionStore.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * ICallSessionStore on profile.db (V011). The reads share one read-only connection, opened on the first read and kept
 * until the store goes (a call reads several times every frame); each write opens its own connection and closes it.
 */
class SqliteCallSessionStore : public ICallSessionStore {
public:
  explicit SqliteCallSessionStore(std::string profile_db_path);

  /** The call tables at the current shape, where missing (a change to them is a ladder step: SqliteChatDbLadders.h). */
  static Roe<void> EnsureSchema(sqlite3* profile_db);

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

  /** Tests: how many times the shared read connection has been opened. */
  size_t ReadConnectionOpensForTest() const;
  /** Tests: runs `sql` on the shared read connection (opening it first); a write there must fail. */
  Roe<void> ExecOnReadConnectionForTest(const std::string& sql) const;
  /** Tests: runs on the reading thread once a read holds the read connection, before its statement. */
  static void SetReadHookForTest(std::function<void()> hook);

private:
  struct ReadConnection;
  class ReadStatement;

  Roe<sqlite3*> OpenDb() const;
  /** The shared read connection, opened (after OpenDb has made the tables) on first use; the caller holds a reference. */
  Roe<std::shared_ptr<ReadConnection>> ReadDb() const;

  std::string profile_db_path_;
  /** The schema runs once per store, not on every short-lived connection. */
  mutable std::mutex schema_mutex_;
  mutable bool schema_ready_ = false;
  /** Guards read_db_ and read_opens_ only; a read runs its statement after letting go. */
  mutable std::mutex read_mutex_;
  /** A read in flight keeps its own reference: the connection closes when the store and the last read let go. */
  mutable std::shared_ptr<ReadConnection> read_db_;
  mutable size_t read_opens_ = 0;
};

} // namespace pbr
