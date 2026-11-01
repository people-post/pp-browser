#include "lib/messaging/ThreadMentionsStore.h"

#include <sqlite3.h>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kThreadMentionsSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS thread_mentions (
  thread_id TEXT NOT NULL,
  message_id TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (thread_id, message_id)
);
)sql";

} // namespace

ThreadMentionsStore::ThreadMentionsStore(std::string profile_db_path) : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> ThreadMentionsStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for thread mentions");
  }
  // Shares the file with SqliteThreadStore's long-lived handle — wait out SQLITE_BUSY.
  sqlite3_busy_timeout(db, 5000);
  if (auto schema = EnsureSchema(db); !schema) {
    sqlite3_close(db);
    return schema.error();
  }
  return db;
}

Roe<void> ThreadMentionsStore::EnsureSchema(sqlite3* profile_db) const {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kThreadMentionsSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "thread mentions schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<void> ThreadMentionsStore::Exec(const char* sql, const std::string& a, const std::string& b,
                                    const int64_t n) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare a thread mentions statement");
  }
  sqlite3_bind_text(stmt, 1, a.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_bind_parameter_count(stmt) >= 2) {
    sqlite3_bind_text(stmt, 2, b.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (sqlite3_bind_parameter_count(stmt) >= 3) {
    sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(n));
  }
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to update thread mentions");
  }
  return {};
}

Roe<void> ThreadMentionsStore::Add(const std::string& thread_id, const std::string& message_id,
                                   const int64_t created_at) const {
  return Exec("INSERT OR IGNORE INTO thread_mentions (thread_id, message_id, created_at) VALUES (?, ?, ?);", thread_id,
              message_id, created_at);
}

Roe<void> ThreadMentionsStore::DropThread(const std::string& thread_id) const {
  return Exec("DELETE FROM thread_mentions WHERE thread_id = ?;", thread_id);
}

Roe<void> ThreadMentionsStore::DropMessage(const std::string& thread_id, const std::string& message_id) const {
  return Exec("DELETE FROM thread_mentions WHERE thread_id = ? AND message_id = ?;", thread_id, message_id);
}

Roe<std::set<std::string>> ThreadMentionsStore::Threads() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT DISTINCT thread_id FROM thread_mentions;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to list thread mentions");
  }
  std::set<std::string> threads;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const unsigned char* id = sqlite3_column_text(stmt, 0)) {
      threads.insert(reinterpret_cast<const char*>(id));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return threads;
}

Roe<std::optional<std::string>> ThreadMentionsStore::Oldest(const std::string& thread_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT message_id FROM thread_mentions WHERE thread_id = ? ORDER BY created_at ASC, message_id ASC LIMIT 1;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to read thread mentions");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<std::string> oldest;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const unsigned char* id = sqlite3_column_text(stmt, 0)) {
      oldest = std::string(reinterpret_cast<const char*>(id));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return oldest;
}

} // namespace pbr
