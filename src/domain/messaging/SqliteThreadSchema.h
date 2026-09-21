#pragma once

#include "common/Error.h"

#include <filesystem>
#include <string>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** profile.db / thread.db user_version — bump with incompatible layout (D102). */
inline constexpr int kSqliteThreadProfileUserVersion = 4;
inline constexpr int kSqliteThreadUserVersion = 2;

inline constexpr const char* kSqliteThreadMessageSelectColumns =
    "id, display_order, sender_contact_id, content_enc, content_type, control_type, timestamp, relay_visible, "
    "delivery, transport, sender_seq, session_epoch, target_message_id, generation, seq_owner_contact_id, "
    "ai_invoke_mode";

inline constexpr const char* kSqliteThreadProfileSchemaV1 = R"sql(
CREATE TABLE IF NOT EXISTS threads (
  id TEXT PRIMARY KEY,
  kind TEXT NOT NULL,
  channel TEXT NOT NULL DEFAULT '',
  group_id TEXT,
  peer_identity_kind TEXT,
  peer_identity_value TEXT,
  title TEXT NOT NULL,
  local_title TEXT NOT NULL DEFAULT '',
  participant_contact_ids TEXT NOT NULL,
  preview_enc BLOB,
  updated_at INTEGER NOT NULL,
  unread_count INTEGER NOT NULL DEFAULT 0,
  session_epoch INTEGER
);
CREATE INDEX IF NOT EXISTS idx_threads_updated ON threads(updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_threads_direct ON threads(kind, channel, peer_identity_kind, peer_identity_value);

CREATE TABLE IF NOT EXISTS outbox (
  message_id TEXT PRIMARY KEY,
  thread_id TEXT NOT NULL,
  delivery TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_outbox_thread ON outbox(thread_id);
CREATE INDEX IF NOT EXISTS idx_outbox_updated ON outbox(updated_at ASC);

CREATE TABLE IF NOT EXISTS chat_targets (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  channel TEXT NOT NULL,
  participant_contact_id TEXT,
  local_thread_id TEXT NOT NULL,
  session_epoch INTEGER NOT NULL DEFAULT 1,
  next_outgoing_seq INTEGER NOT NULL DEFAULT 1,
  master_psk_b64 TEXT,
  psk_fingerprint TEXT,
  psk_verified_at INTEGER,
  retired_psks_json TEXT,
  key_scope TEXT NOT NULL DEFAULT 'account',
  thread_kem_pk_b64 TEXT,
  thread_kem_sk_b64 TEXT,
  peer_thread_kem_pk_b64 TEXT,
  last_psk_rotate_at INTEGER,
  psk_rotate_msg_count INTEGER NOT NULL DEFAULT 0,
  last_rotation_id TEXT,
  PRIMARY KEY (peer_identity_kind, peer_identity_value, channel)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_chat_targets_local_thread ON chat_targets(local_thread_id);
)sql";

inline constexpr const char* kSqliteThreadSchemaV2 = R"sql(
CREATE TABLE IF NOT EXISTS messages (
  id TEXT PRIMARY KEY,
  display_order INTEGER NOT NULL,
  sender_contact_id TEXT NOT NULL,
  content_enc BLOB NOT NULL,
  content_type TEXT NOT NULL,
  control_type TEXT,
  timestamp INTEGER NOT NULL,
  relay_visible INTEGER NOT NULL,
  delivery TEXT NOT NULL,
  transport TEXT,
  sender_seq INTEGER,
  session_epoch INTEGER,
  target_message_id TEXT,
  generation TEXT,
  seq_owner_contact_id TEXT,
  ai_invoke_mode TEXT
);
CREATE INDEX IF NOT EXISTS idx_messages_display ON messages(display_order DESC);
CREATE INDEX IF NOT EXISTS idx_messages_seq ON messages(session_epoch, sender_contact_id, sender_seq)
  WHERE relay_visible = 1;
CREATE INDEX IF NOT EXISTS idx_messages_delivery ON messages(delivery) WHERE relay_visible = 1;

CREATE TABLE IF NOT EXISTS memory (
  key TEXT PRIMARY KEY,
  value_enc BLOB NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_state (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  state_json TEXT NOT NULL,
  PRIMARY KEY (peer_identity_kind, peer_identity_value, session_epoch)
);
)sql";

inline std::string SqliteThreadsRoot(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "threads").string();
}

inline std::string SqliteThreadProfileDbFile(const std::string& data_dir) {
  return (std::filesystem::path(SqliteThreadsRoot(data_dir)) / "profile.db").string();
}

inline std::string SqliteThreadDir(const std::string& data_dir, const std::string& thread_id) {
  return (std::filesystem::path(SqliteThreadsRoot(data_dir)) / thread_id).string();
}

inline std::string SqliteThreadDbFile(const std::string& data_dir, const std::string& thread_id) {
  return (std::filesystem::path(SqliteThreadDir(data_dir, thread_id)) / "thread.db").string();
}

Roe<void> SqliteThreadExecSql(sqlite3* db, const char* sql);
Roe<void> SqliteThreadApplyUserVersion(sqlite3* db, int version);
Roe<void> SqliteThreadConfigureDb(sqlite3* db);

} // namespace pbr
