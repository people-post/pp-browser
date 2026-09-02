#include "domain/messaging/CallSessionStore.h"

#include <sqlite3.h>
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
  return session;
}

} // namespace

CallSessionStore::CallSessionStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> CallSessionStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for call sessions");
  }
  if (auto schema = EnsureSchema(db); !schema) {
    sqlite3_close(db);
    return schema.error();
  }
  return db;
}

Roe<void> CallSessionStore::EnsureSchema(sqlite3* profile_db) const {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kCallSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "call schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  (void)sqlite3_exec(profile_db, "ALTER TABLE call_sessions ADD COLUMN video_allowed INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  (void)sqlite3_exec(profile_db,
                     "ALTER TABLE pending_call_invites ADD COLUMN video_allowed INTEGER NOT NULL DEFAULT 0;",
                     nullptr, nullptr, nullptr);
  return {};
}

Roe<void> CallSessionStore::UpsertSession(const CallSession& session) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO call_sessions (call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
      "created_at, ended_at, media_epoch, media_key_id, sfu_hint) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id) DO UPDATE SET origin_thread_id=excluded.origin_thread_id, "
      "origin_group_id=excluded.origin_group_id, media_mode=excluded.media_mode, "
      "video_allowed=excluded.video_allowed, state=excluded.state, "
      "created_at=excluded.created_at, ended_at=excluded.ended_at, media_epoch=excluded.media_epoch, "
      "media_key_id=excluded.media_key_id, sfu_hint=excluded.sfu_hint;";
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
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert call session");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::optional<CallSession>> CallSessionStore::LoadSession(const std::string& call_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                         "created_at, ended_at, media_epoch, media_key_id, sfu_hint FROM call_sessions WHERE "
                         "call_id = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call session load");
  }
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<CallSession> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    result = SessionFromStmt(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return result;
}

Roe<std::vector<CallSession>> CallSessionStore::ListActiveSessions() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "SELECT call_id, origin_thread_id, origin_group_id, media_mode, video_allowed, state, "
                         "created_at, ended_at, media_epoch, media_key_id, sfu_hint FROM call_sessions WHERE state "
                         "!= 'ended' ORDER BY created_at DESC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare active call list");
  }
  std::vector<CallSession> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows.push_back(SessionFromStmt(stmt));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return rows;
}

Roe<void> CallSessionStore::UpsertParticipant(const CallParticipant& participant) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO call_participants (call_id, identity, state, audio_muted, video_enabled, joined_at, left_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id, identity) DO UPDATE SET state=excluded.state, audio_muted=excluded.audio_muted, "
      "video_enabled=excluded.video_enabled, "
      // Sticky SoftMigrate initiator: keep the earliest non-null joined_at.
      "joined_at=CASE "
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
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert call participant");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::vector<CallParticipant>> CallSessionStore::ListParticipants(const std::string& call_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "SELECT identity, state, audio_muted, video_enabled, joined_at, left_at FROM "
                         "call_participants WHERE call_id = ? ORDER BY identity ASC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call participant list");
  }
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<CallParticipant> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    CallParticipant row;
    row.call_id = call_id;
    row.identity = ColumnText(stmt, 0);
    row.state = CallParticipantStateFromString(ColumnText(stmt, 1));
    row.media.audio_muted = sqlite3_column_int(stmt, 2) != 0;
    row.media.video_enabled = sqlite3_column_int(stmt, 3) != 0;
    row.joined_at = ColumnOptInt64(stmt, 4);
    row.left_at = ColumnOptInt64(stmt, 5);
    rows.push_back(std::move(row));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return rows;
}

Roe<std::optional<CallParticipant>> CallSessionStore::FindParticipant(const std::string& call_id,
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

Roe<size_t> CallSessionStore::CountJoined(const std::string& call_id) const {
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

Roe<void> CallSessionStore::UpsertPendingInvite(const PendingCallInvite& invite) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO pending_call_invites (call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
      "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(call_id, invitee_identity) DO UPDATE SET inviter_identity=excluded.inviter_identity, "
      "media_mode=excluded.media_mode, video_allowed=excluded.video_allowed, "
      "origin_thread_id=excluded.origin_thread_id, "
      "origin_group_id=excluded.origin_group_id, sfu_hint=excluded.sfu_hint, expires_at=excluded.expires_at, "
      "created_at=excluded.created_at, status=excluded.status;";
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
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert pending call invite");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return {};
}

Roe<std::optional<PendingCallInvite>> CallSessionStore::LoadPendingInvite(
    const std::string& call_id, const std::string& invitee_identity) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "SELECT call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
                         "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status FROM "
                         "pending_call_invites WHERE call_id = ? AND invitee_identity = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare pending call invite load");
  }
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<PendingCallInvite> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
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
    result = std::move(invite);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return result;
}

Roe<std::vector<PendingCallInvite>> CallSessionStore::ListPendingInvitesForInvitee(
    const std::string& invitee_identity) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "SELECT call_id, invitee_identity, inviter_identity, media_mode, video_allowed, "
                         "origin_thread_id, origin_group_id, sfu_hint, expires_at, created_at, status FROM "
                         "pending_call_invites WHERE invitee_identity = ? AND status = 'pending' ORDER BY "
                         "created_at DESC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare pending call invite list");
  }
  sqlite3_bind_text(stmt, 1, invitee_identity.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<PendingCallInvite> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
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
    rows.push_back(std::move(invite));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return rows;
}

Roe<void> CallSessionStore::UpdateInviteStatus(const std::string& call_id, const std::string& invitee_identity,
                                               const std::string& status) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "UPDATE pending_call_invites SET status = ? WHERE call_id = ? AND invitee_identity = ?;",
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
