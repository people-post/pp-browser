#include "base/messaging/SqliteThreadStore.h"

#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/MessagingLimits.h"

#include "common/Utilities.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace pbr {

namespace {

constexpr int kProfileUserVersion = 1;
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

Roe<void> ConfigureDb(sqlite3* db) {
  if (auto result = ExecSql(db, "PRAGMA journal_mode=WAL;"); !result) {
    return result.error();
  }
  if (auto result = ExecSql(db, "PRAGMA foreign_keys=ON;"); !result) {
    return result.error();
  }
  return {};
}

std::string ContentTypeToDb(ChatContentType type) {
  return type == ChatContentType::System ? "system" : "text";
}

ChatContentType ContentTypeFromDb(const std::string& value) {
  return value == "system" ? ChatContentType::System : ChatContentType::Text;
}

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
    if (auto version = ApplyUserVersion(profile_db_, kThreadUserVersion); !version) {
      return version.error();
    }
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
  if (sqlite3_prepare_v2(profile_db_, "SELECT id, kind, title, participant_contact_ids, updated_at, unread_count, "
                                       "preview FROM threads ORDER BY updated_at DESC;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to list threads");
  }
  std::vector<Thread> threads;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Thread thread;
    thread.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    thread.kind = ThreadKindFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    thread.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    const nlohmann::json participants =
        nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)), nullptr, false);
    if (participants.is_array()) {
      for (const auto& item : participants) {
        if (item.is_string()) {
          thread.participant_contact_ids.push_back(item.get<std::string>());
        }
      }
    }
    thread.updated_at = sqlite3_column_int64(stmt, 4);
    thread.unread_count = sqlite3_column_int(stmt, 5);
    if (sqlite3_column_text(stmt, 6)) {
      thread.preview = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    }
    threads.push_back(std::move(thread));
  }
  sqlite3_finalize(stmt);
  return threads;
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
  const char* sql =
      "INSERT INTO threads (id, kind, channel, title, participant_contact_ids, preview, updated_at, unread_count) "
      "VALUES (?, ?, '', ?, ?, ?, ?, ?) "
      "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, title=excluded.title, "
      "participant_contact_ids=excluded.participant_contact_ids, preview=excluded.preview, "
      "updated_at=excluded.updated_at, unread_count=excluded.unread_count;";
  if (sqlite3_prepare_v2(profile_db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to upsert thread");
  }
  sqlite3_bind_text(stmt, 1, thread.id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, ThreadKindToString(thread.kind).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, thread.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, participants_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, thread.preview.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, thread.updated_at);
  sqlite3_bind_int(stmt, 7, thread.unread_count);
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
  const char* sql = before_display_order.has_value()
                        ? "SELECT id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
                          "content_rml, user_payload, chat_actions, timestamp, relay_visible, delivery FROM messages "
                          "WHERE display_order < ? ORDER BY display_order DESC LIMIT ?;"
                        : "SELECT id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
                          "content_rml, user_payload, chat_actions, timestamp, relay_visible, delivery FROM messages "
                          "ORDER BY display_order DESC LIMIT ?;";
  auto page = QueryMessages(thread_id, sql, before_display_order, limit);
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
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT id, display_order, sender_contact_id, chat_payload, content_type, payload, text, content_rml, "
      "user_payload, chat_actions, timestamp, relay_visible, delivery FROM messages "
      "WHERE content_type IN ('text', 'system') ORDER BY display_order DESC;";
  if (sqlite3_prepare_v2(*thread_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare context query");
  }

  std::vector<ThreadMessage> selected;
  int char_budget = budget.max_recent_chars;
  int turn_pairs = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (turn_pairs >= budget.max_turn_pairs * 2) {
      break;
    }
    auto message = ReadMessageRow(stmt);
    if (!message) {
      sqlite3_finalize(stmt);
      return message.error();
    }
    message->thread_id = thread_id;
    const int line_size = static_cast<int>(message->text.size() + message->sender_contact_id.size() + 2);
    if (char_budget - line_size < 0) {
      break;
    }
    char_budget -= line_size;
    ++turn_pairs;
    selected.push_back(std::move(*message));
  }
  sqlite3_finalize(stmt);
  std::reverse(selected.begin(), selected.end());
  return selected;
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
  const nlohmann::json payload_json = {{"text", stored.text}};
  const std::string payload_text = payload_json.dump();
  const std::string chat_actions_json = ChatActionsToJson(stored.chat_actions).dump();

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO messages (id, display_order, sender_contact_id, chat_payload, content_type, payload, text, "
      "content_rml, chat_actions, timestamp, relay_visible, delivery) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
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
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to append message");
  }
  sqlite3_finalize(stmt);

  if (auto catalog = UpdateThreadCatalogFromMessage(stored, false); !catalog) {
    return catalog.error();
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
  const nlohmann::json payload_json = {{"text", message.text}};
  const std::string payload_text = payload_json.dump();
  const std::string chat_actions_json = ChatActionsToJson(message.chat_actions).dump();

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "UPDATE messages SET sender_contact_id = ?, chat_payload = ?, content_type = ?, payload = ?, text = ?, "
      "content_rml = ?, chat_actions = ?, timestamp = ?, relay_visible = ?, delivery = ? WHERE id = ?;";
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
  sqlite3_bind_text(stmt, 11, message.id.c_str(), -1, SQLITE_TRANSIENT);
  const bool updated = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(*thread_db) > 0;
  sqlite3_finalize(stmt);
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

  std::lock_guard profile_lock(profile_mutex_);
  auto thread_db = OpenThreadDb(thread_id);
  if (!thread_db) {
    return thread_db.error();
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
  (void)ExecSql(*thread_db, "PRAGMA wal_checkpoint(PASSIVE);");
  return {};
}

Roe<bool> SqliteThreadStore::DeleteThread(const std::string& thread_id) {
  if (auto init = EnsureInitialized(); !init) {
    return init.error();
  }
  std::lock_guard profile_lock(profile_mutex_);
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
