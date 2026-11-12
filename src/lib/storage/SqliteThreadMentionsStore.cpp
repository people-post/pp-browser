#include "lib/storage/SqliteThreadMentionsStore.h"
#include "lib/storage/SqliteChatStoreGate.h"

#include <sqlite3.h>

#include <iterator>

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
CREATE TABLE IF NOT EXISTS thread_mentions_read (
  thread_id TEXT NOT NULL PRIMARY KEY,
  message_id TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT 0
);
)sql";

} // namespace

SqliteThreadMentionsStore::SqliteThreadMentionsStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> SqliteThreadMentionsStore::OpenDb() const {
  if (auto gate = chat_store_gate::RequireOpen(profile_db_path_); !gate) {
    return gate.error();
  }
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

Roe<void> SqliteThreadMentionsStore::EnsureSchema(sqlite3* profile_db) const {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kThreadMentionsSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "thread mentions schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<void> SqliteThreadMentionsStore::Exec(const char* sql, const std::string& a, const std::string& b,
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

Roe<void> SqliteThreadMentionsStore::Add(const std::string& thread_id, const std::string& message_id,
                                         const int64_t created_at) const {
  return Exec("INSERT OR IGNORE INTO thread_mentions (thread_id, message_id, created_at) VALUES (?, ?, ?);", thread_id,
              message_id, created_at);
}

Roe<void> SqliteThreadMentionsStore::ExecThreadScript(const char* const* statements, const size_t count,
                                                      const std::string& thread_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  bool ok = sqlite3_exec(*db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
  for (size_t i = 0; ok && i < count; ++i) {
    sqlite3_stmt* stmt = nullptr;
    ok = sqlite3_prepare_v2(*db, statements[i], -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
      sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
      ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
  }
  sqlite3_exec(*db, ok ? "COMMIT;" : "ROLLBACK;", nullptr, nullptr, nullptr);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to update thread mentions");
  }
  return {};
}

Roe<void> SqliteThreadMentionsStore::MarkThreadRead(const std::string& thread_id) const {
  // The thread's newest mention replaces its read row unless that row is newer; then its unread rows go.
  static constexpr const char* kStatements[] = {
      "INSERT INTO thread_mentions_read (thread_id, message_id, created_at) "
      "SELECT thread_id, message_id, created_at FROM thread_mentions WHERE thread_id = ?1 "
      "ORDER BY created_at DESC, message_id DESC LIMIT 1 "
      "ON CONFLICT(thread_id) DO UPDATE SET message_id = excluded.message_id, created_at = excluded.created_at "
      "WHERE excluded.created_at >= thread_mentions_read.created_at;",
      "DELETE FROM thread_mentions WHERE thread_id = ?1;",
  };
  return ExecThreadScript(kStatements, std::size(kStatements), thread_id);
}

Roe<void> SqliteThreadMentionsStore::DropThread(const std::string& thread_id) const {
  static constexpr const char* kStatements[] = {
      "DELETE FROM thread_mentions WHERE thread_id = ?1;",
      "DELETE FROM thread_mentions_read WHERE thread_id = ?1;",
  };
  return ExecThreadScript(kStatements, std::size(kStatements), thread_id);
}

Roe<void> SqliteThreadMentionsStore::DropMessage(const std::string& thread_id, const std::string& message_id) const {
  if (auto read =
          Exec("DELETE FROM thread_mentions_read WHERE thread_id = ? AND message_id = ?;", thread_id, message_id);
      !read) {
    return read;
  }
  return Exec("DELETE FROM thread_mentions WHERE thread_id = ? AND message_id = ?;", thread_id, message_id);
}

Roe<std::set<std::string>> SqliteThreadMentionsStore::Threads() const {
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

Roe<std::map<std::string, int>> SqliteThreadMentionsStore::Counts() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT thread_id, COUNT(*) FROM thread_mentions GROUP BY thread_id;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to count thread mentions");
  }
  std::map<std::string, int> counts;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (const unsigned char* id = sqlite3_column_text(stmt, 0)) {
      counts[reinterpret_cast<const char*>(id)] = sqlite3_column_int(stmt, 1);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return counts;
}

Roe<std::optional<std::string>> SqliteThreadMentionsStore::Oldest(const std::string& thread_id) const {
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

Roe<std::optional<IThreadMentionsStore::Mention>> SqliteThreadMentionsStore::Latest() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT thread_id, message_id, created_at FROM ("
      "SELECT thread_id, message_id, created_at FROM thread_mentions UNION ALL "
      "SELECT thread_id, message_id, created_at FROM thread_mentions_read) "
      "ORDER BY created_at DESC, message_id DESC LIMIT 1;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to read thread mentions");
  }
  std::optional<Mention> latest;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char* thread = sqlite3_column_text(stmt, 0);
    const unsigned char* message = sqlite3_column_text(stmt, 1);
    if (thread && message) {
      latest = Mention{.thread_id = reinterpret_cast<const char*>(thread),
                       .message_id = reinterpret_cast<const char*>(message),
                       .created_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 2))};
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return latest;
}

} // namespace pbr
