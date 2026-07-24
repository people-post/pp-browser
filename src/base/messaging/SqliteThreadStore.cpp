#include "base/messaging/SqliteThreadStore.h"

#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/ConversationSummaryCodec.h"
#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/SyncStateCodec.h"

#include "common/Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

constexpr int kProfileUserVersion = 2;
constexpr int kThreadUserVersion = 1;

constexpr const char* kProfileSchemaV1 = R"sql(
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
  preview TEXT,
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
  PRIMARY KEY (peer_identity_kind, peer_identity_value, channel)
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_chat_targets_local_thread ON chat_targets(local_thread_id);
)sql";

constexpr const char* kMessageSelectColumns =
    "id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
    "content_rml, user_payload, chat_actions, timestamp, relay_visible, delivery, transport, "
    "sender_seq, session_epoch, target_message_id, generation, seq_owner_contact_id, ai_invoke_mode";

constexpr const char* kThreadSchemaV1 = R"sql(
CREATE TABLE IF NOT EXISTS messages (
  id TEXT PRIMARY KEY,
  display_order INTEGER NOT NULL,
  sender_contact_id TEXT NOT NULL,
  chat_payload BLOB NOT NULL,
  content_type TEXT NOT NULL,
  payload TEXT NOT NULL,
  text TEXT,
  content_rml TEXT,
  user_payload TEXT,
  chat_actions TEXT NOT NULL DEFAULT '[]',
  timestamp INTEGER NOT NULL,
  relay_visible INTEGER NOT NULL,
  delivery TEXT NOT NULL,
  transport TEXT,
  sender_seq INTEGER,
  session_epoch INTEGER,
  target_message_id TEXT,
  generation TEXT,
  seq_owner_contact_id TEXT,
  ai_invoke_mode TEXT,
  control_type TEXT
);
CREATE INDEX IF NOT EXISTS idx_messages_display ON messages(display_order DESC);
CREATE INDEX IF NOT EXISTS idx_messages_seq ON messages(session_epoch, sender_contact_id, sender_seq)
  WHERE relay_visible = 1;
CREATE INDEX IF NOT EXISTS idx_messages_delivery ON messages(delivery) WHERE relay_visible = 1;

CREATE TABLE IF NOT EXISTS memory (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sync_state (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  session_epoch INTEGER NOT NULL,
  state_json TEXT NOT NULL,
  PRIMARY KEY (peer_identity_kind, peer_identity_value, session_epoch)
);
)sql";

std::string ThreadsRoot(const std::string& data_dir) {
  return (std::filesystem::path(data_dir) / "threads").string();
}

std::string ProfileDbFile(const std::string& data_dir) {
  return (std::filesystem::path(ThreadsRoot(data_dir)) / "profile.db").string();
}

std::string ThreadDir(const std::string& data_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadsRoot(data_dir)) / thread_id).string();
}

std::string ThreadDbFile(const std::string& data_dir, const std::string& thread_id) {
  return (std::filesystem::path(ThreadDir(data_dir, thread_id)) / "thread.db").string();
}

Roe<void> ExecSql(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "sqlite exec failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<void> ApplyUserVersion(sqlite3* db, int version) {
  if (auto result = ExecSql(db, ("PRAGMA user_version = " + std::to_string(version) + ";").c_str()); !result) {
    return result.error();
  }
  return {};
}

Roe<void> MigrateProfileDb(sqlite3* db, int from_version) {
  if (from_version < 2) {
    // Additive column for dual group titles (local override).
    if (auto result = ExecSql(db, "ALTER TABLE threads ADD COLUMN local_title TEXT NOT NULL DEFAULT '';"); !result) {
      // Ignore duplicate-column errors from partially migrated DBs.
      const std::string& msg = result.error().message;
      if (msg.find("duplicate column") == std::string::npos) {
        return result.error();
      }
    }
  }
  if (auto version = ApplyUserVersion(db, kProfileUserVersion); !version) {
    return version.error();
  }
  return {};
}

Roe<void> ConfigureDb(sqlite3* db) {
  if (auto result = ExecSql(db, "PRAGMA journal_mode=WAL;"); !result) {
    return result.error();
  }
  if (auto result = ExecSql(db, "PRAGMA foreign_keys=ON;"); !result) {
    return result.error();
  }
  return {};
}

std::string ContentTypeToDb(ChatContentType type) { return ChatContentTypeToDb(type); }

ChatContentType ContentTypeFromDb(const std::string& value) { return ChatContentTypeFromDb(value); }

nlohmann::json ChatActionsToJson(const std::vector<TranscriptChatAction>& actions) {
  nlohmann::json out = nlohmann::json::array();
  for (const TranscriptChatAction& action : actions) {
    nlohmann::json item = {{"label", action.label}, {"message", action.message}};
    if (action.payload) {
      item["payload"] = *action.payload;
    }
    out.push_back(std::move(item));
  }
  return out;
}

std::vector<TranscriptChatAction> ChatActionsFromJsonString(const std::string& json_text) {
  const nlohmann::json parsed = nlohmann::json::parse(json_text, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    return {};
  }
  std::vector<TranscriptChatAction> out;
  for (const auto& item : parsed) {
    TranscriptChatAction action;
    if (item.contains("label") && item["label"].is_string()) {
      action.label = item["label"].get<std::string>();
    }
    if (item.contains("message") && item["message"].is_string()) {
      action.message = item["message"].get<std::string>();
    }
    if (item.contains("payload") && item["payload"].is_string()) {
      action.payload = item["payload"].get<std::string>();
    }
    out.push_back(std::move(action));
  }
  return out;
}

} // namespace

SqliteThreadStore::SqliteThreadStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("SqliteThreadStore");
}

SqliteThreadStore::~SqliteThreadStore() {
  std::lock_guard profile_lock(profile_mutex_);
  for (auto& [thread_id, handle] : thread_dbs_) {
    (void)thread_id;
    if (handle.db) {
      sqlite3_close(handle.db);
      handle.db = nullptr;
    }
  }
  if (profile_db_) {
    sqlite3_close(profile_db_);
    profile_db_ = nullptr;
  }
}

std::string SqliteThreadStore::ProfileDbPath() const {
  return ProfileDbFile(data_dir_);
}

Roe<void> SqliteThreadStore::EnsureInitialized() const {
  if (initialized_) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(ThreadsRoot(data_dir_), ec);
  if (auto wipe = WipeLegacyJsonIfPresent(); !wipe) {
    return wipe.error();
  }
  if (auto open = OpenProfileDb(); !open) {
    return open.error();
  }
  if (auto repair = RepairOrphanThreadDirs(); !repair) {
    return repair.error();
  }
  initialized_ = true;
  return {};
}

Roe<void> SqliteThreadStore::WipeLegacyJsonIfPresent() const {
  const std::filesystem::path index = std::filesystem::path(ThreadsRoot(data_dir_)) / "index.json";
  if (!std::filesystem::exists(index)) {
    return {};
  }
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(ThreadsRoot(data_dir_), ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() == ".json") {
      std::filesystem::remove(entry.path(), ec);
    }
  }
  return {};
}

Roe<void> SqliteThreadStore::OpenProfileDb() const {
  if (profile_db_) {
    return {};
  }
  const bool created = !std::filesystem::exists(ProfileDbFile(data_dir_));
  if (sqlite3_open(ProfileDbFile(data_dir_).c_str(), &profile_db_) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  if (auto cfg = ConfigureDb(profile_db_); !cfg) {
    return cfg.error();
  }
  int user_version = 0;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      user_version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  if (created || user_version == 0) {
    if (auto schema = ExecSql(profile_db_, kProfileSchemaV1); !schema) {
      return schema.error();
    }
    if (auto version = ApplyUserVersion(profile_db_, kProfileUserVersion); !version) {
      return version.error();
    }
  } else if (user_version < kProfileUserVersion) {
    if (auto migrated = MigrateProfileDb(profile_db_, user_version); !migrated) {
      return migrated.error();
    }
  }
  GroupRosterStore roster_store(ProfileDbFile(data_dir_));
  if (auto group_schema = roster_store.EnsureSchema(profile_db_); !group_schema) {
    return group_schema.error();
  }
  return {};
}

Roe<void> SqliteThreadStore::EnsureThreadDirectory(const std::string& thread_id) const {
  std::error_code ec;
  std::filesystem::create_directories(ThreadDir(data_dir_, thread_id), ec);
  std::filesystem::create_directories(std::filesystem::path(ThreadDir(data_dir_, thread_id)) / "blobs", ec);
  return {};
}

Roe<sqlite3*> SqliteThreadStore::OpenThreadDb(const std::string& thread_id) const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard lock(thread_cache_mutex_);
  auto it = thread_dbs_.find(thread_id);
  if (it == thread_dbs_.end() || it->second.db == nullptr) {
    if (auto dir = EnsureThreadDirectory(thread_id); !dir) {
      return dir.error();
    }
    ThreadDbHandle handle;
    if (sqlite3_open(ThreadDbFile(data_dir_, thread_id).c_str(), &handle.db) != SQLITE_OK) {
      return Error("Failed to open thread.db: " + thread_id);
    }
    if (auto cfg = ConfigureDb(handle.db); !cfg) {
      sqlite3_close(handle.db);
      return cfg.error();
    }
    int user_version = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle.db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        user_version = sqlite3_column_int(stmt, 0);
      }
      sqlite3_finalize(stmt);
    }
    if (user_version == 0) {
      if (auto schema = ExecSql(handle.db, kThreadSchemaV1); !schema) {
        sqlite3_close(handle.db);
        return schema.error();
      }
      if (auto version = ApplyUserVersion(handle.db, kThreadUserVersion); !version) {
        sqlite3_close(handle.db);
        return version.error();
      }
    }
    thread_dbs_[thread_id] = handle;
    it = thread_dbs_.find(thread_id);
  }
  TouchThreadLru(thread_id);
  EvictThreadDbsIfNeeded();
  return it->second.db;
}

void SqliteThreadStore::TouchThreadLru(const std::string& thread_id) const {
  thread_lru_.remove(thread_id);
  thread_lru_.push_front(thread_id);
}

void SqliteThreadStore::EvictThreadDbsIfNeeded() const {
  while (thread_dbs_.size() > kMaxOpenThreadDbs && !thread_lru_.empty()) {
    const std::string evict_id = thread_lru_.back();
    thread_lru_.pop_back();
    auto it = thread_dbs_.find(evict_id);
    if (it != thread_dbs_.end()) {
      if (it->second.db) {
        sqlite3_close(it->second.db);
      }
      thread_dbs_.erase(it);
    }
  }
}

Roe<ThreadMessage> SqliteThreadStore::ReadMessageRow(sqlite3_stmt* stmt) const {
  ThreadMessage message;
  message.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  message.display_order = sqlite3_column_int64(stmt, 1);
  message.sender_contact_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  const void* blob = sqlite3_column_blob(stmt, 3);
  const int blob_size = sqlite3_column_bytes(stmt, 3);
  std::vector<uint8_t> chat_payload(static_cast<const uint8_t*>(blob),
                                    static_cast<const uint8_t*>(blob) + blob_size);
  message.content_type = ContentTypeFromDb(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
  if (sqlite3_column_text(stmt, 5)) {
    message.payload_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  }
  if (sqlite3_column_text(stmt, 6)) {
    message.text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  }
  if (sqlite3_column_text(stmt, 7)) {
    message.content_rml = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
  }
  if (sqlite3_column_text(stmt, 9)) {
    message.chat_actions = ChatActionsFromJsonString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9)));
  }
  message.timestamp = sqlite3_column_int64(stmt, 10);
  message.relay_visible = sqlite3_column_int(stmt, 11) != 0;
  message.delivery = MessageDeliveryFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 12)));
  if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) {
    message.transport = MessageTransportFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 13)));
  }
  if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) {
    message.sender_seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 14));
  }
  if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) {
    message.session_epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 15));
  }
  if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) {
    message.target_message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 16));
  }
  if (sqlite3_column_type(stmt, 17) != SQLITE_NULL) {
    message.generation = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 17));
  }
  if (sqlite3_column_type(stmt, 18) != SQLITE_NULL) {
    message.seq_owner_contact_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 18));
  }
  if (sqlite3_column_type(stmt, 19) != SQLITE_NULL) {
    message.ai_invoke_mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 19));
  }
  (void)ChatPayloadCodec::ApplyRowToMessage(chat_payload, message);
  return message;
}

Roe<int64_t> SqliteThreadStore::NextDisplayOrder(sqlite3* thread_db) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(thread_db, "SELECT COALESCE(MAX(display_order), 0) + 1 FROM messages;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return Error("Failed to prepare display_order query");
  }
  int64_t next = 1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    next = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return next;
}

Roe<void> SqliteThreadStore::UpdateThreadCatalogFromMessage(const ThreadMessage& message,
                                                            const bool increment_unread) const {
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE threads SET updated_at = ?, preview = ?, unread_count = unread_count + ? WHERE id = ?;";
  if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare catalog update");
  }
  sqlite3_bind_int64(stmt, 1, message.timestamp);
  sqlite3_bind_text(stmt, 2, message.text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, increment_unread ? 1 : 0);
  sqlite3_bind_text(stmt, 4, message.thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to update thread catalog");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::RepairOrphanThreadDirs() const {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(ThreadsRoot(data_dir_), ec)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string thread_id = entry.path().filename().string();
    if (thread_id == "blobs") {
      continue;
    }
    const std::filesystem::path db_path = entry.path() / "thread.db";
    if (!std::filesystem::exists(db_path)) {
      continue;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(profile_db_, "SELECT 1 FROM threads WHERE id = ? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
      continue;
    }
    sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (exists) {
      continue;
    }
    const int64_t now = util::NowUnixMs();
    if (sqlite3_prepare_v2(profile_db_,
                           "INSERT INTO threads (id, kind, channel, title, participant_contact_ids, preview, "
                           "updated_at, unread_count) VALUES (?, 'ai', '', ?, '[]', '', ?, 0);",
                           -1, &stmt, nullptr) != SQLITE_OK) {
      continue;
    }
    sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, thread_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  return {};
}

void SqliteThreadStore::Flush() {
  std::lock_guard profile_lock(profile_mutex_);
  if (profile_db_) {
    (void)ExecSql(profile_db_, "PRAGMA wal_checkpoint(PASSIVE);");
  }
  std::lock_guard thread_lock(thread_cache_mutex_);
  for (auto& [thread_id, handle] : thread_dbs_) {
    (void)thread_id;
    if (handle.db) {
      (void)ExecSql(handle.db, "PRAGMA wal_checkpoint(PASSIVE);");
    }
  }
}

Roe<std::vector<Thread>> SqliteThreadStore::ListThreads() const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_,
                         "SELECT id, kind, channel, title, participant_contact_ids, updated_at, unread_count, "
                         "preview, peer_identity_kind, peer_identity_value, group_id, local_title FROM threads "
                         "ORDER BY updated_at DESC;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to list threads");
  }
  std::vector<Thread> threads;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    threads.push_back(ReadThreadRow(stmt));
  }
  sqlite3_finalize(stmt);
  return threads;
}

Thread SqliteThreadStore::ReadThreadRow(sqlite3_stmt* stmt) const {
  Thread thread;
  thread.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  thread.kind = ThreadKindFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
  if (sqlite3_column_text(stmt, 2)) {
    thread.channel = ThreadChannelFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
  }
  thread.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  const nlohmann::json participants =
      nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)), nullptr, false);
  if (participants.is_array()) {
    for (const auto& item : participants) {
      if (item.is_string()) {
        thread.participant_contact_ids.push_back(item.get<std::string>());
      }
    }
  }
  thread.updated_at = sqlite3_column_int64(stmt, 5);
  thread.unread_count = sqlite3_column_int(stmt, 6);
  if (sqlite3_column_text(stmt, 7)) {
    thread.preview = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
  }
  if (sqlite3_column_text(stmt, 8)) {
    thread.peer_identity_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
  }
  if (sqlite3_column_text(stmt, 9)) {
    thread.peer_identity_value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
  }
  if (sqlite3_column_text(stmt, 10)) {
    thread.group_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
  }
  if (sqlite3_column_text(stmt, 11)) {
    thread.local_title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
  }
  thread.encrypted = thread.kind == ThreadKind::Group || ThreadChannelIsE2e(thread.channel);
  return thread;
}

Roe<std::optional<Thread>> SqliteThreadStore::GetThread(const std::string& thread_id) const {
  auto threads = ListThreads();
  if (!threads) {
    return threads.error();
  }
  for (const Thread& thread : *threads) {
    if (thread.id == thread_id) {
      return Roe<std::optional<Thread>>(thread);
    }
  }
  return Roe<std::optional<Thread>>(std::optional<Thread>{});
}

Roe<Thread> SqliteThreadStore::UpsertThread(const Thread& thread) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  if (auto dir = EnsureThreadDirectory(thread.id); !dir) {
    return dir.error();
  }
  (void)OpenThreadDb(thread.id);

  std::lock_guard lock(profile_mutex_);
  nlohmann::json participants = nlohmann::json::array();
  for (const std::string& id : thread.participant_contact_ids) {
    participants.push_back(id);
  }
  const std::string participants_json = participants.dump();
  sqlite3_stmt* stmt = nullptr;
  const std::string channel_text = ThreadChannelToString(thread.channel);
  const char* sql =
      "INSERT INTO threads (id, kind, channel, title, local_title, participant_contact_ids, preview, updated_at, "
      "unread_count, peer_identity_kind, peer_identity_value, group_id) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, channel=excluded.channel, title=excluded.title, "
      "local_title=excluded.local_title, "
      "participant_contact_ids=excluded.participant_contact_ids, preview=excluded.preview, "
      "updated_at=excluded.updated_at, unread_count=excluded.unread_count, "
      "peer_identity_kind=excluded.peer_identity_kind, peer_identity_value=excluded.peer_identity_value, "
      "group_id=excluded.group_id;";
  if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to upsert thread");
  }
  sqlite3_bind_text(stmt, 1, thread.id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, ThreadKindToString(thread.kind).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, channel_text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, thread.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, thread.local_title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, participants_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, thread.preview.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, thread.updated_at);
  sqlite3_bind_int(stmt, 9, thread.unread_count);
  sqlite3_bind_text(stmt, 10, thread.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, thread.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  if (thread.group_id) {
    sqlite3_bind_text(stmt, 12, thread.group_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 12);
  }
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to upsert thread row");
  }
  sqlite3_finalize(stmt);
  return thread;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::QueryMessages(const std::string& thread_id, const char* sql,
                                                                 std::optional<int64_t> before_display_order,
                                                                 size_t limit) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare message query");
  }
  int bind_index = 1;
  if (before_display_order.has_value()) {
    sqlite3_bind_int64(stmt, bind_index++, *before_display_order);
  }
  sqlite3_bind_int64(stmt, bind_index, static_cast<int64_t>(limit));

  std::vector<ThreadMessage> messages;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto message = ReadMessageRow(stmt);
    if (!message) {
      sqlite3_finalize(stmt);
      return message.error();
    }
    message->thread_id = thread_id;
    messages.push_back(std::move(*message));
  }
  sqlite3_finalize(stmt);
  return messages;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::GetMessagesPage(const std::string& thread_id,
                                                                  std::optional<int64_t> before_display_order,
                                                                  size_t limit) const {
  if (limit == 0) {
    limit = kDefaultMessagesPageSize;
  }
  const std::string select_prefix = std::string("SELECT ") + kMessageSelectColumns + " FROM messages ";
  const std::string sql = before_display_order.has_value()
                                ? select_prefix + "WHERE display_order < ? ORDER BY display_order DESC LIMIT ?;"
                                : select_prefix + "ORDER BY display_order DESC LIMIT ?;";
  auto page = QueryMessages(thread_id, sql.c_str(), before_display_order, limit);
  if (!page) {
    return page.error();
  }
  std::reverse(page->begin(), page->end());
  return page;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::GetMessages(const std::string& thread_id) const {
  return GetMessagesPage(thread_id, std::nullopt, 1000000);
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::GetMessagesForContext(const std::string& thread_id,
                                                                         const ContextBudget& budget) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }

  int64_t compaction_cursor = 0;
  if (auto memory = GetThreadMemory(thread_id)) {
    if (memory->has_value() && memory->value().compacted_through_display_order.has_value()) {
      compaction_cursor = *memory->value().compacted_through_display_order;
    }
  } else {
    return memory.error();
  }

  sqlite3_stmt* stmt = nullptr;
  const std::string context_sql = std::string("SELECT ") + kMessageSelectColumns +
                                  " FROM messages WHERE content_type IN ('text', 'system') AND display_order > ? "
                                  "ORDER BY display_order DESC;";
  if (sqlite3_prepare_v2(*thread_db, context_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare context query");
  }
  sqlite3_bind_int64(stmt, 1, compaction_cursor);

  std::vector<ThreadMessage> fetched;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto message = ReadMessageRow(stmt);
    if (!message) {
      sqlite3_finalize(stmt);
      return message.error();
    }
    message->thread_id = thread_id;
    fetched.push_back(std::move(*message));
  }
  sqlite3_finalize(stmt);

  const size_t min_keep = static_cast<size_t>(kCompactionMinTurnsKept * 2);
  std::vector<ThreadMessage> selected;
  int char_budget = budget.max_recent_chars;
  for (size_t i = 0; i < fetched.size(); ++i) {
    const ThreadMessage& message = fetched[i];
    const bool below_min = selected.size() < min_keep;
    if (!below_min && static_cast<int>(selected.size()) >= budget.max_turn_pairs * 2) {
      break;
    }
    const int line_size = static_cast<int>(message.text.size() + message.sender_contact_id.size() + 2);
    if (!below_min && char_budget - line_size < 0) {
      break;
    }
    char_budget -= line_size;
    selected.push_back(message);
  }
  std::reverse(selected.begin(), selected.end());
  return selected;
}

Roe<std::optional<ConversationSummary>> SqliteThreadStore::GetThreadMemory(const std::string& thread_id) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db, "SELECT value FROM memory WHERE key = ? LIMIT 1;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Error("Failed to prepare memory read");
  }
  sqlite3_bind_text(stmt, 1, ConversationSummaryCodec::kSummaryKey, -1, SQLITE_STATIC);
  std::optional<ConversationSummary> summary;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (value) {
      auto decoded = ConversationSummaryCodec::Decode(value);
      if (!decoded) {
        sqlite3_finalize(stmt);
        return decoded.error();
      }
      summary = std::move(*decoded);
    }
  }
  sqlite3_finalize(stmt);
  return summary;
}

Roe<void> SqliteThreadStore::SetThreadMemory(const std::string& thread_id, const ConversationSummary& summary) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto encoded = ConversationSummaryCodec::Encode(summary);
  if (!encoded) {
    return encoded.error();
  }
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO memory (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare memory write");
  }
  sqlite3_bind_text(stmt, 1, ConversationSummaryCodec::kSummaryKey, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, encoded->c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to write memory");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::ClearThreadMemory(const std::string& thread_id) {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db, "DELETE FROM memory WHERE key = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare memory clear");
  }
  sqlite3_bind_text(stmt, 1, ConversationSummaryCodec::kSummaryKey, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to clear memory");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<int64_t> SqliteThreadStore::CountContextEligibleMessagesAfter(const std::string& thread_id,
                                                                  const int64_t after_display_order) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT COUNT(*) FROM messages WHERE content_type IN ('text', 'system') AND display_order > ?;";
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare compaction count");
  }
  sqlite3_bind_int64(stmt, 1, after_display_order);
  int64_t count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::GetContextEligibleMessagesAfter(
    const std::string& thread_id, const int64_t after_display_order) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const std::string compaction_sql = std::string("SELECT ") + kMessageSelectColumns +
                                     " FROM messages WHERE content_type IN ('text', 'system') AND display_order > ? "
                                     "ORDER BY display_order ASC;";
  if (sqlite3_prepare_v2(*thread_db, compaction_sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare compaction query");
  }
  sqlite3_bind_int64(stmt, 1, after_display_order);

  std::vector<ThreadMessage> messages;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto message = ReadMessageRow(stmt);
    if (!message) {
      sqlite3_finalize(stmt);
      return message.error();
    }
    message->thread_id = thread_id;
    messages.push_back(std::move(*message));
  }
  sqlite3_finalize(stmt);
  return messages;
}

Roe<ThreadMessage> SqliteThreadStore::AppendMessage(const ThreadMessage& message) {
  if (message.text.size() > kMaxComposeTextBytes) {
    return Error("Message text exceeds compose limit");
  }
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto thread_db = OpenThreadDb(message.thread_id);
  if (!thread_db) {
    return thread_db.error();
  }

  ThreadMessage stored = message;
  if (stored.display_order <= 0) {
    auto next = NextDisplayOrder(*thread_db);
    if (!next) {
      return next.error();
    }
    stored.display_order = *next;
  }

  auto chat_payload = ChatPayloadCodec::EncodeToRow(stored);
  if (!chat_payload) {
    return chat_payload.error();
  }
  const std::string payload_text =
      stored.payload_json.empty() ? ChatPayloadCodec::BuildPayloadJson(stored) : stored.payload_json;
  const std::string chat_actions_json = ChatActionsToJson(stored.chat_actions).dump();

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO messages (id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
      "content_rml, chat_actions, timestamp, relay_visible, delivery, transport, sender_seq, session_epoch, "
      "target_message_id, generation, seq_owner_contact_id, ai_invoke_mode) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare append");
  }
  sqlite3_bind_text(stmt, 1, stored.id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, stored.display_order);
  sqlite3_bind_text(stmt, 3, stored.sender_contact_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 4, chat_payload->data(), static_cast<int>(chat_payload->size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, ContentTypeToDb(stored.content_type).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, payload_text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, stored.text.c_str(), -1, SQLITE_TRANSIENT);
  if (stored.content_rml) {
    sqlite3_bind_text(stmt, 8, stored.content_rml->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  sqlite3_bind_text(stmt, 9, chat_actions_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 10, stored.timestamp);
  sqlite3_bind_int(stmt, 11, stored.relay_visible ? 1 : 0);
  sqlite3_bind_text(stmt, 12, MessageDeliveryToString(stored.delivery).c_str(), -1, SQLITE_TRANSIENT);
  if (stored.transport) {
    sqlite3_bind_text(stmt, 13, MessageTransportToString(*stored.transport).c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 13);
  }
  if (stored.sender_seq) {
    sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(*stored.sender_seq));
  } else {
    sqlite3_bind_null(stmt, 14);
  }
  if (stored.session_epoch) {
    sqlite3_bind_int(stmt, 15, static_cast<int>(*stored.session_epoch));
  } else {
    sqlite3_bind_null(stmt, 15);
  }
  if (stored.target_message_id) {
    sqlite3_bind_text(stmt, 16, stored.target_message_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 16);
  }
  if (stored.generation) {
    sqlite3_bind_text(stmt, 17, stored.generation->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 17);
  }
  if (stored.seq_owner_contact_id) {
    sqlite3_bind_text(stmt, 18, stored.seq_owner_contact_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 18);
  }
  if (stored.ai_invoke_mode) {
    sqlite3_bind_text(stmt, 19, stored.ai_invoke_mode->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 19);
  }
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to append message");
  }
  sqlite3_finalize(stmt);

  if (auto catalog = UpdateThreadCatalogFromMessage(stored, false); !catalog) {
    return catalog.error();
  }
  if (stored.delivery == MessageDelivery::Pending && stored.relay_visible) {
    if (auto outbox = UpsertOutboxRow(stored.id, stored.thread_id); !outbox) {
      return outbox.error();
    }
  }
  return stored;
}

Roe<bool> SqliteThreadStore::UpdateMessage(const ThreadMessage& message) {
  auto thread_db = OpenThreadDb(message.thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  auto chat_payload = ChatPayloadCodec::EncodeToRow(message);
  if (!chat_payload) {
    return chat_payload.error();
  }
  const std::string payload_text =
      message.payload_json.empty() ? ChatPayloadCodec::BuildPayloadJson(message) : message.payload_json;
  const std::string chat_actions_json = ChatActionsToJson(message.chat_actions).dump();

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE messages SET sender_contact_id = ?, chat_payload = ?, content_type = ?, payload = ?, text = ?, "
      "content_rml = ?, chat_actions = ?, timestamp = ?, relay_visible = ?, delivery = ?, transport = ?, "
      "sender_seq = ?, session_epoch = ?, target_message_id = ?, generation = ?, seq_owner_contact_id = ?, "
      "ai_invoke_mode = ? WHERE id = ?;";
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, message.sender_contact_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, chat_payload->data(), static_cast<int>(chat_payload->size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ContentTypeToDb(message.content_type).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, payload_text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, message.text.c_str(), -1, SQLITE_TRANSIENT);
  if (message.content_rml) {
    sqlite3_bind_text(stmt, 6, message.content_rml->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  sqlite3_bind_text(stmt, 7, chat_actions_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 8, message.timestamp);
  sqlite3_bind_int(stmt, 9, message.relay_visible ? 1 : 0);
  sqlite3_bind_text(stmt, 10, MessageDeliveryToString(message.delivery).c_str(), -1, SQLITE_TRANSIENT);
  if (message.transport) {
    sqlite3_bind_text(stmt, 11, MessageTransportToString(*message.transport).c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 11);
  }
  if (message.sender_seq) {
    sqlite3_bind_int64(stmt, 12, static_cast<sqlite3_int64>(*message.sender_seq));
  } else {
    sqlite3_bind_null(stmt, 12);
  }
  if (message.session_epoch) {
    sqlite3_bind_int(stmt, 13, static_cast<int>(*message.session_epoch));
  } else {
    sqlite3_bind_null(stmt, 13);
  }
  if (message.target_message_id) {
    sqlite3_bind_text(stmt, 14, message.target_message_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 14);
  }
  if (message.generation) {
    sqlite3_bind_text(stmt, 15, message.generation->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 15);
  }
  if (message.seq_owner_contact_id) {
    sqlite3_bind_text(stmt, 16, message.seq_owner_contact_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 16);
  }
  if (message.ai_invoke_mode) {
    sqlite3_bind_text(stmt, 17, message.ai_invoke_mode->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 17);
  }
  sqlite3_bind_text(stmt, 18, message.id.c_str(), -1, SQLITE_TRANSIENT);
  const bool updated = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(*thread_db) > 0;
  sqlite3_finalize(stmt);
  if (updated && message.delivery != MessageDelivery::Pending) {
    (void)RemoveOutboxRow(message.id);
  }
  return updated;
}

Roe<bool> SqliteThreadStore::HasMessageId(const std::string& thread_id, const std::string& message_id) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db, "SELECT 1 FROM messages WHERE id = ? LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare dedup query");
  }
  sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return found;
}

Roe<void> SqliteThreadStore::ClearMessages(const std::string& thread_id, const ClearMessagesOptions& options) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }

  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }

  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }

  if ((*thread)->kind == ThreadKind::Direct && ThreadChannelIsE2e((*thread)->channel)) {
    auto session_epoch = GetChatTargetSessionEpoch(thread_id);
    if (session_epoch) {
      uint64_t loaded_max_seq = 0;
      sqlite3_stmt* max_stmt = nullptr;
      if (sqlite3_prepare_v2(*thread_db,
                             "SELECT MAX(sender_seq) FROM messages WHERE sender_contact_id != ? AND sender_seq IS "
                             "NOT NULL AND session_epoch = ?;",
                             -1, &max_stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(max_stmt, 1, kLocalSelfContactId, -1, SQLITE_STATIC);
        sqlite3_bind_int(max_stmt, 2, static_cast<int>(*session_epoch));
        if (sqlite3_step(max_stmt) == SQLITE_ROW && sqlite3_column_type(max_stmt, 0) != SQLITE_NULL) {
          loaded_max_seq = static_cast<uint64_t>(sqlite3_column_int64(max_stmt, 0));
        }
        sqlite3_finalize(max_stmt);
      }

      auto sync_state = GetPeerSyncState(thread_id, *session_epoch);
      if (sync_state) {
        PeerSyncState updated = *sync_state;
        updated.history_floor_seq = loaded_max_seq;
        updated.contiguous_peer_seq = 0;
        updated.loaded_min_seq = 0;
        updated.loaded_max_seq = 0;
        updated.phase = PeerSyncPhase::Ok;
        updated.empty_closed_seqs.clear();
        updated.empty_closed_ranges.clear();
        (void)SetPeerSyncState(thread_id, *session_epoch, updated);
      }
    }
  }

  if (sqlite3_exec(*thread_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    return Error("Failed to begin thread txn");
  }
  if (sqlite3_exec(*thread_db, "DELETE FROM messages;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    sqlite3_exec(*thread_db, "ROLLBACK;", nullptr, nullptr, nullptr);
    return Error("Failed to clear messages");
  }
  if (options.forget_memory) {
    if (sqlite3_exec(*thread_db, "DELETE FROM memory;", nullptr, nullptr, nullptr) != SQLITE_OK) {
      sqlite3_exec(*thread_db, "ROLLBACK;", nullptr, nullptr, nullptr);
      return Error("Failed to clear memory");
    }
  }
  if (sqlite3_exec(*thread_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
    return Error("Failed to commit clear");
  }

  {
    std::lock_guard profile_lock(profile_mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(profile_db_, "DELETE FROM outbox WHERE thread_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(profile_db_, "UPDATE threads SET preview = '', unread_count = 0 WHERE id = ?;", -1, &stmt,
                           nullptr) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  (void)ExecSql(*thread_db, "PRAGMA wal_checkpoint(PASSIVE);");
  return {};
}

Roe<bool> SqliteThreadStore::DeleteThread(const std::string& thread_id) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard profile_lock(profile_mutex_);
  ClearChatTargetThreadLinkUnlocked(thread_id);
  {
    GroupRosterStore roster(ProfileDbFile(data_dir_));
    (void)roster.ClearGroupTargetByThreadId(thread_id);
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "DELETE FROM threads WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  if (sqlite3_prepare_v2(profile_db_, "DELETE FROM outbox WHERE thread_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  CloseThreadDb(thread_id);
  std::error_code ec;
  std::filesystem::remove_all(ThreadDir(data_dir_, thread_id), ec);
  return true;
}

Roe<void> SqliteThreadStore::UpsertChatTarget(const DirectChatTarget& target,
                                              const std::string& participant_contact_id,
                                              const std::string& local_thread_id) const {
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, participant_contact_id, "
      "local_thread_id, session_epoch, next_outgoing_seq) "
      "VALUES (?, ?, ?, ?, ?, 1, 1) "
      "ON CONFLICT(peer_identity_kind, peer_identity_value, channel) DO UPDATE SET "
      "participant_contact_id=excluded.participant_contact_id, local_thread_id=excluded.local_thread_id;";
  if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare chat_targets upsert");
  }
  const std::string channel = ThreadChannelToString(target.channel);
  sqlite3_bind_text(stmt, 1, target.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, channel.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, participant_contact_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, local_thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to upsert chat_targets");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::ClearChatTargetThreadLink(const std::string& thread_id) const {
  std::lock_guard lock(profile_mutex_);
  ClearChatTargetThreadLinkUnlocked(thread_id);
  return {};
}

void SqliteThreadStore::ClearChatTargetThreadLinkUnlocked(const std::string& thread_id) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "UPDATE chat_targets SET local_thread_id = '' WHERE local_thread_id = ?;", -1,
                         &stmt, nullptr) != SQLITE_OK) {
    return;
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

Roe<void> SqliteThreadStore::UpsertOutboxRow(const std::string& message_id, const std::string& thread_id) const {
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO outbox (message_id, thread_id, delivery, updated_at) VALUES (?, ?, 'pending', ?) "
      "ON CONFLICT(message_id) DO UPDATE SET thread_id=excluded.thread_id, delivery='pending', "
      "updated_at=excluded.updated_at;";
  if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare outbox upsert");
  }
  sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, util::NowUnixMs());
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to upsert outbox row");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::RemoveOutboxRow(const std::string& message_id) const {
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "DELETE FROM outbox WHERE message_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare outbox delete");
  }
  sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return {};
}

Roe<std::optional<Thread>> SqliteThreadStore::FindDirectThread(const DirectChatTarget& target) const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::optional<std::string> thread_id;
  {
    std::lock_guard lock(profile_mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "SELECT local_thread_id FROM chat_targets WHERE peer_identity_kind = ? AND peer_identity_value = ? AND "
        "channel = ? AND local_thread_id != '' LIMIT 1;";
    if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return Error("Failed to prepare chat_targets lookup");
    }
    const std::string channel = ThreadChannelToString(target.channel);
    sqlite3_bind_text(stmt, 1, target.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, target.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, channel.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
      thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }
  if (!thread_id) {
    return Roe<std::optional<Thread>>(std::optional<Thread>{});
  }
  return GetThread(*thread_id);
}

Roe<Thread> SqliteThreadStore::FindOrCreateDirectThread(const DirectChatTarget& target,
                                                        const std::string& participant_contact_id,
                                                        const std::string& title) {
  if (auto existing = FindDirectThread(target)) {
    if (!existing) {
      return existing.error();
    }
    if (*existing) {
      if (ThreadChannelIsE2e(target.channel)) {
        if (auto epoch = GetChatTargetSessionEpoch((*existing)->id)) {
          (void)EnsurePeerSyncState((*existing)->id, target, *epoch);
        }
      }
      return **existing;
    }
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Direct;
  thread.channel = target.channel;
  thread.peer_identity_kind = target.peer_identity_kind;
  thread.peer_identity_value = target.peer_identity_value;
  thread.participant_contact_ids = {participant_contact_id};
  thread.title = title;
  thread.encrypted = ThreadChannelIsE2e(target.channel);
  thread.updated_at = util::NowUnixMs();

  auto saved = UpsertThread(thread);
  if (!saved) {
    return saved.error();
  }

  if (auto target_row = UpsertChatTarget(target, participant_contact_id, saved->id); !target_row) {
    return target_row.error();
  }
  if (ThreadChannelIsE2e(target.channel)) {
    if (auto epoch = GetChatTargetSessionEpoch(saved->id)) {
      if (auto sync = EnsurePeerSyncState(saved->id, target, *epoch); !sync) {
        return sync.error();
      }
    }
  }
  return *saved;
}

Roe<std::optional<Thread>> SqliteThreadStore::FindGroupThread(const std::string& group_id) const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  GroupRosterStore roster(ProfileDbFile(data_dir_));
  auto mapped_thread_id = roster.FindThreadIdForGroup(group_id);
  if (!mapped_thread_id) {
    return mapped_thread_id.error();
  }
  std::optional<std::string> thread_id = mapped_thread_id.value();
  if (!thread_id) {
    std::lock_guard lock(profile_mutex_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(profile_db_, "SELECT id FROM threads WHERE group_id = ? LIMIT 1;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
      return Error("Failed to prepare group thread lookup");
    }
    sqlite3_bind_text(stmt, 1, group_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0)) {
      thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }
  if (!thread_id) {
    return Roe<std::optional<Thread>>(std::optional<Thread>{});
  }
  return GetThread(*thread_id);
}

Roe<Thread> SqliteThreadStore::FindOrCreateGroupThread(const std::string& group_id, const std::string& title,
                                                        const std::vector<std::string>& participant_contact_ids) {
  if (auto existing = FindGroupThread(group_id)) {
    if (!existing) {
      return existing.error();
    }
    if (*existing) {
      return **existing;
    }
  }

  Thread thread;
  thread.id = util::GenerateUuid();
  thread.kind = ThreadKind::Group;
  thread.channel = ThreadChannel::None;
  thread.group_id = group_id;
  thread.participant_contact_ids = participant_contact_ids;
  thread.title = title;
  thread.encrypted = true;
  thread.updated_at = util::NowUnixMs();

  auto saved = UpsertThread(thread);
  if (!saved) {
    return saved.error();
  }

  GroupRosterStore roster(ProfileDbFile(data_dir_));
  if (auto target = roster.UpsertGroupTarget(group_id, saved->id, 1, 1); !target) {
    return target.error();
  }
  return *saved;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::ExportMessagesUpTo(
    const std::string& thread_id, const std::optional<std::string>& max_message_id) const {
  auto messages = GetMessages(thread_id);
  if (!messages) {
    return messages.error();
  }
  if (!max_message_id) {
    return *messages;
  }
  std::vector<ThreadMessage> exported;
  for (const ThreadMessage& message : *messages) {
    exported.push_back(message);
    if (message.id == *max_message_id) {
      break;
    }
  }
  return exported;
}

Roe<uint64_t> SqliteThreadStore::AllocateSenderSeq(const std::string& thread_id) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    GroupRosterStore roster(ProfileDbFile(data_dir_));
    return roster.AllocateGroupSenderSeq(*(*thread)->group_id);
  }
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "SELECT next_outgoing_seq FROM chat_targets WHERE local_thread_id = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sender seq lookup");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return Error("No chat target for thread");
  }
  const uint64_t seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(profile_db_, "UPDATE chat_targets SET next_outgoing_seq = next_outgoing_seq + 1 WHERE "
                                     "local_thread_id = ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sender seq bump");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to bump sender seq");
  }
  sqlite3_finalize(stmt);
  return seq;
}

Roe<uint32_t> SqliteThreadStore::GetChatTargetSessionEpoch(const std::string& thread_id) const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  if ((*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    GroupRosterStore roster(ProfileDbFile(data_dir_));
    return roster.GetGroupSessionEpoch(*(*thread)->group_id);
  }
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "SELECT session_epoch FROM chat_targets WHERE local_thread_id = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare session epoch lookup");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return Error("No chat target for thread");
  }
  const uint32_t epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
  sqlite3_finalize(stmt);
  return epoch;
}

Roe<DirectChatTarget> SqliteThreadStore::DirectTargetForThread(const Thread& thread) const {
  if (thread.kind != ThreadKind::Direct) {
    return Error("Not a direct thread");
  }
  if (thread.peer_identity_kind.empty() || thread.peer_identity_value.empty()) {
    return Error("Direct thread missing peer identity");
  }
  DirectChatTarget target;
  target.peer_identity_kind = thread.peer_identity_kind;
  target.peer_identity_value = thread.peer_identity_value;
  target.channel = thread.channel;
  return target;
}

Roe<void> SqliteThreadStore::EnsurePeerSyncState(const std::string& thread_id, const DirectChatTarget& target,
                                                 const uint32_t session_epoch) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db,
                         "SELECT 1 FROM sync_state WHERE peer_identity_kind = ? AND peer_identity_value = ? AND "
                         "session_epoch = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sync_state lookup");
  }
  sqlite3_bind_text(stmt, 1, target.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if (exists) {
    return {};
  }

  const std::string state_json = PeerSyncStateToJson(DefaultPeerSyncState());
  if (sqlite3_prepare_v2(*thread_db,
                         "INSERT INTO sync_state (peer_identity_kind, peer_identity_value, session_epoch, state_json) "
                         "VALUES (?, ?, ?, ?);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sync_state insert");
  }
  sqlite3_bind_text(stmt, 1, target.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  sqlite3_bind_text(stmt, 4, state_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to insert sync_state");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<PeerSyncState> SqliteThreadStore::GetPeerSyncState(const std::string& thread_id,
                                                       const uint32_t session_epoch) const {
  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  auto target = DirectTargetForThread(**thread);
  if (!target) {
    return target.error();
  }

  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db,
                         "SELECT state_json FROM sync_state WHERE peer_identity_kind = ? AND peer_identity_value = ? "
                         "AND session_epoch = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sync_state read");
  }
  sqlite3_bind_text(stmt, 1, target->peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target->peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return Error("sync_state not found");
  }
  const std::string json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return PeerSyncStateFromJson(json);
}

Roe<void> SqliteThreadStore::SetPeerSyncState(const std::string& thread_id, const uint32_t session_epoch,
                                              const PeerSyncState& state) {
  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  auto target = DirectTargetForThread(**thread);
  if (!target) {
    return target.error();
  }

  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  const std::string state_json = PeerSyncStateToJson(state);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db,
                         "INSERT INTO sync_state (peer_identity_kind, peer_identity_value, session_epoch, state_json) "
                         "VALUES (?, ?, ?, ?) "
                         "ON CONFLICT(peer_identity_kind, peer_identity_value, session_epoch) DO UPDATE SET "
                         "state_json=excluded.state_json;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sync_state upsert");
  }
  sqlite3_bind_text(stmt, 1, target->peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target->peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  sqlite3_bind_text(stmt, 4, state_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to upsert sync_state");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::CancelOldEpochPendingUnlocked(sqlite3* thread_db, const std::string& thread_id,
                                                           const uint32_t old_session_epoch) const {
  std::vector<std::string> message_ids;
  sqlite3_stmt* select_stmt = nullptr;
  const char* select_sql =
      "SELECT id FROM messages WHERE relay_visible = 1 AND session_epoch = ? AND delivery IN ('pending', 'failed');";
  if (sqlite3_prepare_v2(thread_db, select_sql, -1, &select_stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare old-epoch pending select");
  }
  sqlite3_bind_int(select_stmt, 1, static_cast<int>(old_session_epoch));
  while (sqlite3_step(select_stmt) == SQLITE_ROW) {
    if (sqlite3_column_text(select_stmt, 0)) {
      message_ids.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(select_stmt, 0)));
    }
  }
  sqlite3_finalize(select_stmt);

  for (const std::string& message_id : message_ids) {
    sqlite3_stmt* outbox_stmt = nullptr;
    if (sqlite3_prepare_v2(profile_db_, "DELETE FROM outbox WHERE message_id = ?;", -1, &outbox_stmt, nullptr) ==
        SQLITE_OK) {
      sqlite3_bind_text(outbox_stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(outbox_stmt);
      sqlite3_finalize(outbox_stmt);
    }
  }

  sqlite3_stmt* delete_stmt = nullptr;
  const char* delete_sql =
      "DELETE FROM messages WHERE relay_visible = 1 AND session_epoch = ? AND delivery IN ('pending', 'failed');";
  if (sqlite3_prepare_v2(thread_db, delete_sql, -1, &delete_stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare old-epoch pending delete");
  }
  sqlite3_bind_int(delete_stmt, 1, static_cast<int>(old_session_epoch));
  if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
    sqlite3_finalize(delete_stmt);
    return Error("Failed to delete old-epoch pending messages");
  }
  sqlite3_finalize(delete_stmt);
  (void)thread_id;
  return {};
}

Roe<void> SqliteThreadStore::AdoptChatTargetEpochUnlocked(const std::string& thread_id,
                                                          const uint32_t new_session_epoch) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_,
                         "UPDATE chat_targets SET session_epoch = ?, next_outgoing_seq = 1 WHERE local_thread_id = ?;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare chat_targets epoch adopt");
  }
  sqlite3_bind_int(stmt, 1, static_cast<int>(new_session_epoch));
  sqlite3_bind_text(stmt, 2, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to adopt chat target epoch");
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(profile_db_, "UPDATE threads SET session_epoch = ? WHERE id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Error("Failed to prepare threads epoch cache update");
  }
  sqlite3_bind_int(stmt, 1, static_cast<int>(new_session_epoch));
  sqlite3_bind_text(stmt, 2, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to update threads epoch cache");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::UpsertPeerSyncStateUnlocked(sqlite3* thread_db, const DirectChatTarget& target,
                                                         const uint32_t session_epoch,
                                                         const PeerSyncState& state) const {
  const std::string state_json = PeerSyncStateToJson(state);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(thread_db,
                         "INSERT INTO sync_state (peer_identity_kind, peer_identity_value, session_epoch, state_json) "
                         "VALUES (?, ?, ?, ?) "
                         "ON CONFLICT(peer_identity_kind, peer_identity_value, session_epoch) DO UPDATE SET "
                         "state_json=excluded.state_json;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare sync_state upsert");
  }
  sqlite3_bind_text(stmt, 1, target.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, static_cast<int>(session_epoch));
  sqlite3_bind_text(stmt, 4, state_json.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to upsert sync_state");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<void> SqliteThreadStore::CancelOldEpochPending(const std::string& thread_id, const uint32_t old_session_epoch) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard profile_lock(profile_mutex_);
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }
  return CancelOldEpochPendingUnlocked(*thread_db, thread_id, old_session_epoch);
}

Roe<void> SqliteThreadStore::AdoptChatTargetEpoch(const std::string& thread_id, const uint32_t new_session_epoch) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard profile_lock(profile_mutex_);
  return AdoptChatTargetEpochUnlocked(thread_id, new_session_epoch);
}

Roe<ThreadMessage> SqliteThreadStore::AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message,
                                                                       const uint32_t old_session_epoch,
                                                                       const uint32_t new_session_epoch,
                                                                       const PeerSyncState& new_sync_state) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto thread = GetThread(message.thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  auto target = DirectTargetForThread(**thread);
  if (!target) {
    return target.error();
  }

  ThreadMessage stored = message;
  {
    std::lock_guard profile_lock(profile_mutex_);
    auto thread_db = OpenThreadDb(message.thread_id);
    if (!thread_db) {
      return thread_db.error();
    }

    if (auto cancelled = CancelOldEpochPendingUnlocked(*thread_db, message.thread_id, old_session_epoch); !cancelled) {
      return cancelled.error();
    }
    if (auto adopted = AdoptChatTargetEpochUnlocked(message.thread_id, new_session_epoch); !adopted) {
      return adopted.error();
    }
    if (auto sync = UpsertPeerSyncStateUnlocked(*thread_db, *target, new_session_epoch, new_sync_state); !sync) {
      return sync.error();
    }

    if (stored.display_order <= 0) {
      auto next = NextDisplayOrder(*thread_db);
      if (!next) {
        return next.error();
      }
      stored.display_order = *next;
    }

    auto chat_payload = ChatPayloadCodec::EncodeToRow(stored);
    if (!chat_payload) {
      return chat_payload.error();
    }
    const std::string payload_text =
        stored.payload_json.empty() ? ChatPayloadCodec::BuildPayloadJson(stored) : stored.payload_json;
    const std::string chat_actions_json = ChatActionsToJson(stored.chat_actions).dump();

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
        "content_rml, chat_actions, timestamp, relay_visible, delivery, transport, sender_seq, session_epoch, "
        "target_message_id, generation, seq_owner_contact_id, ai_invoke_mode) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
      return Error("Failed to prepare passive-adopt append");
    }
    sqlite3_bind_text(stmt, 1, stored.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, stored.display_order);
    sqlite3_bind_text(stmt, 3, stored.sender_contact_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, chat_payload->data(), static_cast<int>(chat_payload->size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, ContentTypeToDb(stored.content_type).c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, payload_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, stored.text.c_str(), -1, SQLITE_TRANSIENT);
    if (stored.content_rml) {
      sqlite3_bind_text(stmt, 8, stored.content_rml->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 8);
    }
    sqlite3_bind_text(stmt, 9, chat_actions_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 10, stored.timestamp);
    sqlite3_bind_int(stmt, 11, stored.relay_visible ? 1 : 0);
    sqlite3_bind_text(stmt, 12, MessageDeliveryToString(stored.delivery).c_str(), -1, SQLITE_TRANSIENT);
    if (stored.transport) {
      sqlite3_bind_text(stmt, 13, MessageTransportToString(*stored.transport).c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 13);
    }
    if (stored.sender_seq) {
      sqlite3_bind_int64(stmt, 14, static_cast<sqlite3_int64>(*stored.sender_seq));
    } else {
      sqlite3_bind_null(stmt, 14);
    }
    if (stored.session_epoch) {
      sqlite3_bind_int(stmt, 15, static_cast<int>(*stored.session_epoch));
    } else {
      sqlite3_bind_null(stmt, 15);
    }
    if (stored.target_message_id) {
      sqlite3_bind_text(stmt, 16, stored.target_message_id->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 16);
    }
    if (stored.generation) {
      sqlite3_bind_text(stmt, 17, stored.generation->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 17);
    }
    if (stored.seq_owner_contact_id) {
      sqlite3_bind_text(stmt, 18, stored.seq_owner_contact_id->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 18);
    }
    if (stored.ai_invoke_mode) {
      sqlite3_bind_text(stmt, 19, stored.ai_invoke_mode->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_null(stmt, 19);
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      return Error("Failed to append message during passive adopt");
    }
    sqlite3_finalize(stmt);
  }

  if (auto catalog = UpdateThreadCatalogFromMessage(stored, false); !catalog) {
    return catalog.error();
  }
  return stored;
}

Roe<uint32_t> SqliteThreadStore::BumpLocalChatTargetEpoch(const std::string& thread_id) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto thread = GetThread(thread_id);
  if (!thread) {
    return thread.error();
  }
  if (!*thread) {
    return Error("Thread not found");
  }
  auto target = DirectTargetForThread(**thread);
  if (!target) {
    return target.error();
  }

  const auto old_epoch = GetChatTargetSessionEpoch(thread_id);
  if (!old_epoch) {
    return old_epoch.error();
  }
  const uint32_t new_epoch = *old_epoch + 1;

  std::lock_guard profile_lock(profile_mutex_);
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }

  if (auto cancelled = CancelOldEpochPendingUnlocked(*thread_db, thread_id, *old_epoch); !cancelled) {
    return cancelled.error();
  }
  if (auto adopted = AdoptChatTargetEpochUnlocked(thread_id, new_epoch); !adopted) {
    return adopted.error();
  }
  if (auto sync = UpsertPeerSyncStateUnlocked(*thread_db, *target, new_epoch, DefaultPeerSyncState()); !sync) {
    return sync.error();
  }
  return new_epoch;
}

Roe<std::vector<ThreadMessage>> SqliteThreadStore::GetMessagesBySeqRange(const std::string& thread_id,
                                                                         const SeqRangeQuery& query) const {
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
  }

  const char* order_clause = query.ascending ? "ASC" : "DESC";
  std::string sql = std::string("SELECT ") + kMessageSelectColumns +
                    " FROM messages WHERE relay_visible = 1 AND session_epoch = ? AND sender_contact_id = ? AND "
                    "sender_seq IS NOT NULL";
  if (query.min_sender_seq) {
    sql += " AND sender_seq >= ?";
  }
  if (query.max_sender_seq) {
    sql += " AND sender_seq <= ?";
  }
  sql += " ORDER BY sender_seq ";
  sql += order_clause;
  sql += " LIMIT ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*thread_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare seq range query");
  }
  int bind_index = 1;
  sqlite3_bind_int(stmt, bind_index++, static_cast<int>(query.session_epoch));
  sqlite3_bind_text(stmt, bind_index++, query.seq_owner_contact_id.c_str(), -1, SQLITE_TRANSIENT);
  if (query.min_sender_seq) {
    sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(*query.min_sender_seq));
  }
  if (query.max_sender_seq) {
    sqlite3_bind_int64(stmt, bind_index++, static_cast<sqlite3_int64>(*query.max_sender_seq));
  }
  sqlite3_bind_int64(stmt, bind_index, static_cast<sqlite3_int64>(query.limit));

  std::vector<ThreadMessage> messages;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto message = ReadMessageRow(stmt);
    if (!message) {
      sqlite3_finalize(stmt);
      return message.error();
    }
    message->thread_id = thread_id;
    messages.push_back(std::move(*message));
  }
  sqlite3_finalize(stmt);
  return messages;
}

Roe<void> SqliteThreadStore::ReconcileOutbox() {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  auto pending_rows = ListPendingOutbox();
  if (!pending_rows) {
    return pending_rows.error();
  }
  for (const auto& [message_id, thread_id] : *pending_rows) {
    auto messages = GetMessagesPage(thread_id, std::nullopt, 10000);
    bool found = false;
    MessageDelivery delivery = MessageDelivery::Pending;
    if (messages) {
      for (const ThreadMessage& message : *messages) {
        if (message.id == message_id) {
          found = true;
          delivery = message.delivery;
          break;
        }
      }
    }
    if (!found || delivery == MessageDelivery::Relayed || delivery == MessageDelivery::Failed) {
      (void)RemoveOutboxRow(message_id);
    }
  }

  auto threads = ListThreads();
  if (!threads) {
    return threads.error();
  }
  for (const Thread& thread : *threads) {
    if (thread.kind != ThreadKind::Direct) {
      continue;
    }
    auto messages = GetMessagesPage(thread.id, std::nullopt, 10000);
    if (!messages) {
      continue;
    }
    for (const ThreadMessage& message : *messages) {
      if (message.delivery != MessageDelivery::Pending || !message.relay_visible) {
        continue;
      }
      bool in_outbox = false;
      if (pending_rows) {
        for (const auto& row : *pending_rows) {
          if (row.first == message.id) {
            in_outbox = true;
            break;
          }
        }
      }
      if (!in_outbox) {
        if (auto outbox = UpsertOutboxRow(message.id, thread.id); !outbox) {
          return outbox.error();
        }
      }
    }
  }
  return {};
}

void SqliteThreadStore::CloseThreadDb(const std::string& thread_id) const {
  std::lock_guard lock(thread_cache_mutex_);
  auto it = thread_dbs_.find(thread_id);
  if (it != thread_dbs_.end()) {
    if (it->second.db) {
      sqlite3_close(it->second.db);
    }
    thread_dbs_.erase(it);
  }
  thread_lru_.remove(thread_id);
}

Roe<std::vector<std::pair<std::string, std::string>>> SqliteThreadStore::ListPendingOutbox() const {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard lock(profile_mutex_);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(profile_db_, "SELECT message_id, thread_id FROM outbox ORDER BY updated_at ASC;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    return Error("Failed to list outbox");
  }
  std::vector<std::pair<std::string, std::string>> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
                      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
  }
  sqlite3_finalize(stmt);
  return rows;
}

} // namespace pbr
