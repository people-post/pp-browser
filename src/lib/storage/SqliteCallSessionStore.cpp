#include "lib/storage/SqliteCallSessionStore.h"

#include "common/Logger.h"

#include <sqlite3.h>
#include <functional>
#include <memory>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kCallSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS call_sessions (
  call_id TEXT PRIMARY KEY,
  origin_thread_id TEXT,
  origin_group_id TEXT,
  media_mode TEXT NOT NULL DEFAULT 'voice',
  video_allowed INTEGER NOT NULL DEFAULT 0,
  state TEXT NOT NULL DEFAULT 'ringing',
  created_at INTEGER NOT NULL,
  ended_at INTEGER,
  media_epoch INTEGER NOT NULL DEFAULT 1,
  media_key_id TEXT NOT NULL DEFAULT '',
  sfu_hint TEXT
);
CREATE INDEX IF NOT EXISTS idx_call_sessions_state ON call_sessions(state);

CREATE TABLE IF NOT EXISTS call_participants (
  call_id TEXT NOT NULL,
  identity TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'invited',
  audio_muted INTEGER NOT NULL DEFAULT 0,
  video_enabled INTEGER NOT NULL DEFAULT 0,
  joined_at INTEGER,
  left_at INTEGER,
  on_hold INTEGER NOT NULL DEFAULT 0,
  video_paused INTEGER NOT NULL DEFAULT 0,
  brought_by TEXT NOT NULL DEFAULT '',
  PRIMARY KEY (call_id, identity)
);
CREATE INDEX IF NOT EXISTS idx_call_participants_state ON call_participants(call_id, state);

CREATE TABLE IF NOT EXISTS pending_call_invites (
  call_id TEXT NOT NULL,
  invitee_identity TEXT NOT NULL,
  inviter_identity TEXT NOT NULL,
  media_mode TEXT NOT NULL DEFAULT 'voice',
  video_allowed INTEGER NOT NULL DEFAULT 0,
  origin_thread_id TEXT,
  origin_group_id TEXT,
  sfu_hint TEXT,
  expires_at INTEGER,
  created_at INTEGER NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  PRIMARY KEY (call_id, invitee_identity)
);
CREATE INDEX IF NOT EXISTS idx_pending_call_invites_invitee ON pending_call_invites(invitee_identity, status);

CREATE TABLE IF NOT EXISTS call_media_keys (
  call_id TEXT NOT NULL,
  media_epoch INTEGER NOT NULL,
  media_key_id TEXT NOT NULL,
  ciphertext BLOB NOT NULL,
  PRIMARY KEY (call_id, media_epoch)
);
)sql";

void BindOptText(sqlite3_stmt* stmt, int index, const std::optional<std::string>& value) {
  if (value) {
    sqlite3_bind_text(stmt, index, value->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, index);
  }
}

void BindOptInt64(sqlite3_stmt* stmt, int index, const std::optional<int64_t>& value) {
  if (value) {
    sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(*value));
  } else {
    sqlite3_bind_null(stmt, index);
  }
}

std::optional<std::string> ColumnOptText(sqlite3_stmt* stmt, int index) {
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    return std::nullopt;
  }
  const unsigned char* text = sqlite3_column_text(stmt, index);
  return text ? std::string(reinterpret_cast<const char*>(text)) : std::string{};
}

std::string ColumnText(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  return text ? std::string(reinterpret_cast<const char*>(text)) : std::string{};
}

logging::Logger& StoreLog() {
  static logging::Logger logger = logging::getLogger("SqliteCallSessionStore");
  return logger;
}

/** A step that ended a read with neither a row nor the end: which read, SQLite's result code and its words. */
std::string StepFailure(const char* read, sqlite3* db, const int rc) {
  return std::string(read) + ": sqlite step failed rc=" + std::to_string(rc) + " (" + sqlite3_errstr(rc) + ": " +
         sqlite3_errmsg(db) + ")";
}

/** The reads that still take a failed step as the end of their rows (or as "not found"): the failure is logged. */
void WarnIfStepFailed(const char* read, sqlite3* db, const int rc) {
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    StoreLog().warning << StepFailure(read, db, rc);
  }
}

std::optional<int64_t> ColumnOptInt64(sqlite3_stmt* stmt, int index) {
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
    return std::nullopt;
  }
  return static_cast<int64_t>(sqlite3_column_int64(stmt, index));
}

CallSession SessionFromStmt(sqlite3_stmt* stmt) {
  CallSession session;
  session.call_id = ColumnText(stmt, 0);
  session.origin_thread_id = ColumnOptText(stmt, 1);
  session.origin_group_id = ColumnOptText(stmt, 2);
  session.media_mode = CallMediaModeFromString(ColumnText(stmt, 3));
  session.video_allowed = sqlite3_column_int(stmt, 4) != 0;
  session.state = CallSessionStateFromString(ColumnText(stmt, 5));
  session.created_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 6));
  session.ended_at = ColumnOptInt64(stmt, 7);
  session.media_epoch = static_cast<uint32_t>(sqlite3_column_int64(stmt, 8));
  session.media_key_id = ColumnText(stmt, 9);
  session.sfu_hint = ColumnOptText(stmt, 10);
  // Optional additive column (idx 11) — missing on older SELECTs stays Group.
  if (sqlite3_column_count(stmt) > 11) {
    session.session_kind = CallSessionKindFromString(ColumnText(stmt, 11));
  }
  // V050 planned hop (idx 12, 13).
  if (sqlite3_column_count(stmt) > 13) {
    if (auto peer_id = ColumnOptText(stmt, 12); peer_id && !peer_id->empty()) {
      session.planned_hop = CallPlannedHop{*peer_id, ColumnOptText(stmt, 13).value_or("")};
    }
  }
  if (sqlite3_column_count(stmt) > 14) {
    session.initiator_identity = ColumnText(stmt, 14);
  }
  return session;
}

std::function<void()>& ReadHook() {
  static std::function<void()> hook;
  return hook;
}

} // namespace

/** The shared read-only connection; closed when the store and every read in flight have let go of it. */
struct SqliteCallSessionStore::ReadConnection {
  explicit ReadConnection(sqlite3* opened) : db(opened) {}
  ReadConnection(const ReadConnection&) = delete;
  ReadConnection& operator=(const ReadConnection&) = delete;
  ~ReadConnection() { sqlite3_close_v2(db); }

  sqlite3* const db;
  /**
   * One statement at a time on the connection. Statements overlapping on one connection share a read transaction,
   * so reads from two threads chained back to back could keep reading an old snapshot; one at a time, each read
   * starts its own and sees the newest commit. It also keeps sqlite3_errmsg the read's own.
   */
  std::mutex statement_mutex;
};

/**
 * One read on the shared connection: holds a reference to it and its statement lock for the read, and finalizes the
 * statement on every return (an unfinalized statement would hold the connection on an old snapshot).
 */
class SqliteCallSessionStore::ReadStatement {
public:
  ReadStatement(std::shared_ptr<ReadConnection> connection, const char* sql) : connection_(std::move(connection)) {
    if (ReadHook()) {
      ReadHook()();
    }
    lock_ = std::unique_lock<std::mutex>(connection_->statement_mutex);
    if (sqlite3_prepare_v2(connection_->db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }
  ReadStatement(const ReadStatement&) = delete;
  ReadStatement& operator=(const ReadStatement&) = delete;
  // Finalize under the lock; then the lock goes, then the reference (members go in reverse order).
  ~ReadStatement() { sqlite3_finalize(stmt_); }

  explicit operator bool() const { return stmt_ != nullptr; }
  sqlite3_stmt* Stmt() const { return stmt_; }
  sqlite3* Db() const { return connection_->db; }

private:
  std::shared_ptr<ReadConnection> connection_;
  std::unique_lock<std::mutex> lock_;
  sqlite3_stmt* stmt_ = nullptr;
};

SqliteCallSessionStore::SqliteCallSessionStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<std::shared_ptr<SqliteCallSessionStore::ReadConnection>> SqliteCallSessionStore::ReadDb() const {
  std::lock_guard<std::mutex> lock(read_mutex_);
  if (read_db_) {
    return read_db_;
  }
  // A read-only connection cannot create the file or the tables: the writers' path makes them first (a fresh
  // profile.db reads as empty, as when every read opened that way), and opens the WAL once.
  auto writer = OpenDb();
  if (!writer) {
    return writer.error();
  }
  sqlite3_close(*writer);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(profile_db_path_.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(db); // a failed open still hands back a handle; none is kept half-open
    return Error("Failed to open profile.db for call session reads");
  }
  sqlite3_busy_timeout(db, 5000);
  ++read_opens_;
  read_db_ = std::make_shared<ReadConnection>(db);
  return read_db_;
}

size_t SqliteCallSessionStore::ReadConnectionOpensForTest() const {
  std::lock_guard<std::mutex> lock(read_mutex_);
  return read_opens_;
}

Roe<void> SqliteCallSessionStore::ExecOnReadConnectionForTest(const std::string& sql) const {
  auto connection = ReadDb();
  if (!connection) {
    return connection.error();
  }
  std::lock_guard<std::mutex> lock((*connection)->statement_mutex);
  char* err = nullptr;
  if (sqlite3_exec((*connection)->db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "exec failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

void SqliteCallSessionStore::SetReadHookForTest(std::function<void()> hook) {
  ReadHook() = std::move(hook);
}

Roe<sqlite3*> SqliteCallSessionStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for call sessions");
  }
  // Concurrent AcceptInvite / inbound invite / media-key writers share this file with
  // SqliteThreadStore's long-lived profile.db handle — wait out SQLITE_BUSY on Windows CI.
  sqlite3_busy_timeout(db, 5000);
  std::lock_guard<std::mutex> lock(schema_mutex_);
  if (!schema_ready_) {
    if (auto schema = EnsureSchema(db); !schema) {
      sqlite3_close(db);
      return schema.error();
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<void> SqliteCallSessionStore::EnsureSchema(sqlite3* profile_db) {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kCallSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "call schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN video_allowed INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db,
                     "ALTER TABLE pending_call_invites ADD COLUMN video_allowed INTEGER NOT NULL DEFAULT 0;", nullptr,
                     nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN session_kind TEXT NOT NULL DEFAULT 'group';",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db,
                     "ALTER TABLE pending_call_invites ADD COLUMN session_kind TEXT NOT NULL DEFAULT 'group';", nullptr,
                     nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN planned_hop TEXT;", nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN planned_hop_ma TEXT;", nullptr, nullptr,
                     nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN initiator_identity TEXT NOT NULL DEFAULT '';",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_participants ADD COLUMN on_hold INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_participants ADD COLUMN video_paused INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_participants ADD COLUMN brought_by TEXT NOT NULL DEFAULT '';",
                     nullptr, nullptr, nullptr);
  return {};
}

Roe<void> SqliteCallSessionStore::UpsertSession(const CallSession& session) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO call_sessions (call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
      "created_at, ended_at, media_epoch, media_key_id, sfu_hint, session_kind, planned_hop, planned_hop_ma, "
      "initiator_identity) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id) DO UPDATE SET origin_thread_id=excluded.origin_thread_id, "
      "origin_group_id=excluded.origin_group_id, media_mode=excluded.media_mode, "
      "video_allowed=excluded.video_allowed, state=excluded.state, "
      "created_at=excluded.created_at, ended_at=excluded.ended_at, media_epoch=excluded.media_epoch, "
      "media_key_id=excluded.media_key_id, sfu_hint=excluded.sfu_hint, "
      "session_kind=excluded.session_kind, planned_hop=excluded.planned_hop, "
      "planned_hop_ma=excluded.planned_hop_ma, initiator_identity=excluded.initiator_identity;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call session upsert");
  }
  sqlite3_bind_text(stmt, 1, session.call_id.c_str(), -1, SQLITE_TRANSIENT);
  BindOptText(stmt, 2, session.origin_thread_id);
  BindOptText(stmt, 3, session.origin_group_id);
  const std::string media_mode = CallMediaModeToString(session.media_mode);
  const std::string state = CallSessionStateToString(session.state);
  sqlite3_bind_text(stmt, 4, media_mode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, session.video_allowed ? 1 : 0);
  sqlite3_bind_text(stmt, 6, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(session.created_at));
  BindOptInt64(stmt, 8, session.ended_at);
  sqlite3_bind_int64(stmt, 9, static_cast<sqlite3_int64>(session.media_epoch));
  sqlite3_bind_text(stmt, 10, session.media_key_id.c_str(), -1, SQLITE_TRANSIENT);
  BindOptText(stmt, 11, session.sfu_hint);
  const std::string session_kind = CallSessionKindToString(session.session_kind);
  sqlite3_bind_text(stmt, 12, session_kind.c_str(), -1, SQLITE_TRANSIENT);
  BindOptText(stmt, 13, session.planned_hop ? std::optional<std::string>(session.planned_hop->peer_id) : std::nullopt);
  BindOptText(stmt, 14,
              session.planned_hop ? std::optional<std::string>(session.planned_hop->multiaddr) : std::nullopt);
  sqlite3_bind_text(stmt, 15, session.initiator_identity.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert call session");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::optional<CallSession>> SqliteCallSessionStore::LoadSession(const std::string& call_id) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                           "created_at, ended_at, media_epoch, media_key_id, sfu_hint, session_kind, planned_hop, "
                           "planned_hop_ma, initiator_identity FROM call_sessions WHERE "
                           "call_id = ? LIMIT 1;");
  if (!read) {
    return Error("Failed to prepare call session load");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<CallSession> result;
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    result = SessionFromStmt(stmt);
  }
  WarnIfStepFailed("LoadSession", read.Db(), rc);
  return result;
}

Roe<std::vector<CallSession>> SqliteCallSessionStore::ListActiveSessions() const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                           "created_at, ended_at, media_epoch, media_key_id, sfu_hint, session_kind, planned_hop, "
                           "planned_hop_ma, initiator_identity FROM call_sessions WHERE state "
                           "!= 'ended' ORDER BY created_at DESC;");
  if (!read) {
    return Error("Failed to prepare active call list");
  }
  sqlite3_stmt* stmt = read.Stmt();
  std::vector<CallSession> rows;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    rows.push_back(SessionFromStmt(stmt));
  }
  if (rc != SQLITE_DONE) {
    // The rows read so far are not the active calls: a failed read is not "no call" (the call screen trusts it).
    return Error(StepFailure("ListActiveSessions", read.Db(), rc));
  }
  return rows;
}

Roe<std::vector<CallSession>> SqliteCallSessionStore::ListSessionsForGroup(const std::string& group_id,
                                                                           const size_t limit) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                           "created_at, ended_at, media_epoch, media_key_id, sfu_hint, session_kind, planned_hop, "
                           "planned_hop_ma, initiator_identity FROM call_sessions WHERE origin_group_id = ? "
                           "ORDER BY created_at DESC LIMIT ?;");
  if (!read) {
    return Error("Failed to prepare group call list");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(limit));
  std::vector<CallSession> rows;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    rows.push_back(SessionFromStmt(stmt));
  }
  WarnIfStepFailed("ListSessionsForGroup", read.Db(), rc);
  return rows;
}

Roe<std::vector<CallSession>> SqliteCallSessionStore::ListRecentSessions(const size_t limit) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                           "created_at, ended_at, media_epoch, media_key_id, sfu_hint, session_kind, planned_hop, "
                           "planned_hop_ma, initiator_identity FROM call_sessions ORDER BY created_at DESC LIMIT ?;");
  if (!read) {
    return Error("Failed to prepare recent call list");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(limit));
  std::vector<CallSession> rows;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    rows.push_back(SessionFromStmt(stmt));
  }
  WarnIfStepFailed("ListRecentSessions", read.Db(), rc);
  return rows;
}

Roe<void> SqliteCallSessionStore::UpsertParticipant(const CallParticipant& participant) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO call_participants (call_id, identity, state, audio_muted, video_enabled, joined_at, left_at, "
      "on_hold, video_paused, brought_by) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id, identity) DO UPDATE SET state=excluded.state, audio_muted=excluded.audio_muted, "
      "video_enabled=excluded.video_enabled, on_hold=excluded.on_hold, video_paused=excluded.video_paused, "
      // V062: who brought a guest is kept once known (a later write of the row without it leaves it).
      "brought_by=CASE WHEN excluded.brought_by = '' THEN call_participants.brought_by ELSE excluded.brought_by END, "
      // Sticky SoftMigrate initiator: keep the earliest non-null joined_at — within a stay. Coming back
      // after leaving starts a new stay with the new stamp (B102, #410: the first stay's stamp won, so the
      // roster of whoever let them back in carried it, and those who stayed took the rejoin for a stale
      // roster). Stale rosters never get here: the roster merge wants a stamp newer than the one kept.
      "joined_at=CASE "
      "WHEN call_participants.state = 'left' AND excluded.state IN ('joined', 'invited', 'ringing') "
      "THEN excluded.joined_at "
      "WHEN excluded.joined_at IS NULL THEN call_participants.joined_at "
      "WHEN call_participants.joined_at IS NULL THEN excluded.joined_at "
      "WHEN excluded.joined_at < call_participants.joined_at THEN excluded.joined_at "
      "ELSE call_participants.joined_at END, "
      "left_at=excluded.left_at;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call participant upsert");
  }
  sqlite3_bind_text(stmt, 1, participant.call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, participant.identity.c_str(), -1, SQLITE_TRANSIENT);
  const std::string state = CallParticipantStateToString(participant.state);
  sqlite3_bind_text(stmt, 3, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 4, participant.media.audio_muted ? 1 : 0);
  sqlite3_bind_int(stmt, 5, participant.media.video_enabled ? 1 : 0);
  BindOptInt64(stmt, 6, participant.joined_at);
  BindOptInt64(stmt, 7, participant.left_at);
  sqlite3_bind_int(stmt, 8, participant.media.on_hold ? 1 : 0);
  sqlite3_bind_int(stmt, 9, participant.media.video_paused ? 1 : 0);
  sqlite3_bind_text(stmt, 10, participant.brought_by.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert call participant");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::vector<CallParticipant>> SqliteCallSessionStore::ListParticipants(const std::string& call_id) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(
      std::move(*db),
      "SELECT identity, state, audio_muted, video_enabled, joined_at, left_at, on_hold, video_paused, brought_by "
      "FROM call_participants WHERE call_id = ? ORDER BY identity ASC;");
  if (!read) {
    return Error("Failed to prepare call participant list");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<CallParticipant> rows;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    CallParticipant row;
    row.call_id = call_id;
    row.identity = ColumnText(stmt, 0);
    row.state = CallParticipantStateFromString(ColumnText(stmt, 1));
    row.media.audio_muted = sqlite3_column_int(stmt, 2) != 0;
    row.media.video_enabled = sqlite3_column_int(stmt, 3) != 0;
    row.joined_at = ColumnOptInt64(stmt, 4);
    row.left_at = ColumnOptInt64(stmt, 5);
    row.media.on_hold = sqlite3_column_int(stmt, 6) != 0;
    row.media.video_paused = sqlite3_column_int(stmt, 7) != 0;
    row.brought_by = ColumnText(stmt, 8);
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) {
    // Part of a call's people is not its people: FindParticipant / CountJoined answer from these rows.
    return Error(StepFailure("ListParticipants", read.Db(), rc));
  }
  return rows;
}

Roe<std::optional<CallParticipant>> SqliteCallSessionStore::FindParticipant(const std::string& call_id,
                                                                            const std::string& identity) const {
  auto all = ListParticipants(call_id);
  if (!all) {
    return all.error();
  }
  for (const CallParticipant& row : *all) {
    if (row.identity == identity) {
      return std::optional<CallParticipant>{row};
    }
  }
  return std::optional<CallParticipant>{};
}

Roe<size_t> SqliteCallSessionStore::CountJoined(const std::string& call_id) const {
  auto all = ListParticipants(call_id);
  if (!all) {
    return all.error();
  }
  size_t count = 0;
  for (const CallParticipant& row : *all) {
    if (row.state == CallParticipantState::Joined) {
      ++count;
    }
  }
  return count;
}

Roe<void> SqliteCallSessionStore::UpsertPendingInvite(const PendingCallInvite& invite) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO pending_call_invites (call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
      "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status, session_kind) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id, invitee_identity) DO UPDATE SET inviter_identity=excluded.inviter_identity, "
      "media_mode=excluded.media_mode, video_allowed=excluded.video_allowed, "
      "origin_thread_id=excluded.origin_thread_id, "
      "origin_group_id=excluded.origin_group_id, sfu_hint=excluded.sfu_hint, expires_at=excluded.expires_at, "
      "created_at=excluded.created_at, status=excluded.status, "
      "session_kind=excluded.session_kind;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare pending call invite upsert");
  }
  sqlite3_bind_text(stmt, 1, invite.call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, invite.invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, invite.inviter_identity.c_str(), -1, SQLITE_TRANSIENT);
  const std::string media_mode = CallMediaModeToString(invite.media_mode);
  sqlite3_bind_text(stmt, 4, media_mode.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, invite.video_allowed ? 1 : 0);
  BindOptText(stmt, 6, invite.origin_thread_id);
  BindOptText(stmt, 7, invite.origin_group_id);
  BindOptText(stmt, 8, invite.sfu_hint);
  BindOptInt64(stmt, 9, invite.expires_at);
  sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(invite.created_at));
  sqlite3_bind_text(stmt, 11, invite.status.c_str(), -1, SQLITE_TRANSIENT);
  const std::string session_kind = CallSessionKindToString(invite.session_kind);
  sqlite3_bind_text(stmt, 12, session_kind.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    const std::string detail = sqlite3_errmsg(*db);
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert pending call invite: " + detail);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::optional<PendingCallInvite>> SqliteCallSessionStore::LoadPendingInvite(
    const std::string& call_id, const std::string& invitee_identity) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
                           "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status, session_kind "
                           "FROM "
                           "pending_call_invites WHERE call_id = ? AND invitee_identity = ? LIMIT 1;");
  if (!read) {
    return Error("Failed to prepare pending call invite load");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<PendingCallInvite> result;
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    PendingCallInvite invite;
    invite.call_id = ColumnText(stmt, 0);
    invite.invitee_identity = ColumnText(stmt, 1);
    invite.inviter_identity = ColumnText(stmt, 2);
    invite.media_mode = CallMediaModeFromString(ColumnText(stmt, 3));
    invite.video_allowed = sqlite3_column_int(stmt, 4) != 0;
    invite.origin_thread_id = ColumnOptText(stmt, 5);
    invite.origin_group_id = ColumnOptText(stmt, 6);
    invite.sfu_hint = ColumnOptText(stmt, 7);
    invite.expires_at = ColumnOptInt64(stmt, 8);
    invite.created_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 9));
    invite.status = ColumnText(stmt, 10);
    invite.session_kind = CallSessionKindFromString(ColumnText(stmt, 11));
    result = std::move(invite);
  }
  WarnIfStepFailed("LoadPendingInvite", read.Db(), rc);
  return result;
}

Roe<std::vector<PendingCallInvite>> SqliteCallSessionStore::ListPendingInvitesForInvitee(
    const std::string& invitee_identity) const {
  auto db = ReadDb();
  if (!db) {
    return db.error();
  }
  const ReadStatement read(std::move(*db),
                           "SELECT call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
                           "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status, session_kind "
                           "FROM "
                           "pending_call_invites WHERE invitee_identity = ? AND status = 'pending' ORDER BY "
                           "created_at DESC;");
  if (!read) {
    return Error("Failed to prepare pending call invite list");
  }
  sqlite3_stmt* stmt = read.Stmt();
  sqlite3_bind_text(stmt, 1, invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<PendingCallInvite> rows;
  int rc = SQLITE_ROW;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    PendingCallInvite invite;
    invite.call_id = ColumnText(stmt, 0);
    invite.invitee_identity = ColumnText(stmt, 1);
    invite.inviter_identity = ColumnText(stmt, 2);
    invite.media_mode = CallMediaModeFromString(ColumnText(stmt, 3));
    invite.video_allowed = sqlite3_column_int(stmt, 4) != 0;
    invite.origin_thread_id = ColumnOptText(stmt, 5);
    invite.origin_group_id = ColumnOptText(stmt, 6);
    invite.sfu_hint = ColumnOptText(stmt, 7);
    invite.expires_at = ColumnOptInt64(stmt, 8);
    invite.created_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 9));
    invite.status = ColumnText(stmt, 10);
    invite.session_kind = CallSessionKindFromString(ColumnText(stmt, 11));
    rows.push_back(std::move(invite));
  }
  WarnIfStepFailed("ListPendingInvitesForInvitee", read.Db(), rc);
  return rows;
}

Roe<void> SqliteCallSessionStore::UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                                                     const std::string& status) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "UPDATE pending_call_invites SET status = ? WHERE call_id = ? AND invitee_identity = ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare pending call invite status update");
  }
  sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to update pending call invite status");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

} // namespace pbr
