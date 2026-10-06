#include "domain/messaging/ThreadLocalPrefsStore.h"

#include <sqlite3.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char* kThreadLocalPrefsSchemaSql = R"sql(
CREATE TABLE IF NOT EXISTS thread_local_prefs (
  thread_id TEXT PRIMARY KEY,
  pinned_at INTEGER NOT NULL DEFAULT 0,
  muted_until INTEGER NOT NULL DEFAULT 0,
  archived INTEGER NOT NULL DEFAULT 0
);
)sql";

ThreadLocalPrefs PrefsFromStmt(sqlite3_stmt* stmt) {
  ThreadLocalPrefs prefs;
  const unsigned char* id = sqlite3_column_text(stmt, 0);
  prefs.thread_id = id ? std::string(reinterpret_cast<const char*>(id)) : std::string{};
  prefs.pinned_at = static_cast<int64_t>(sqlite3_column_int64(stmt, 1));
  prefs.muted_until = static_cast<int64_t>(sqlite3_column_int64(stmt, 2));
  prefs.archived = sqlite3_column_int(stmt, 3) != 0;
  return prefs;
}

} // namespace

ThreadLocalPrefsStore::ThreadLocalPrefsStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> ThreadLocalPrefsStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for thread prefs");
  }
  // Shares the file with SqliteThreadStore's long-lived handle — wait out SQLITE_BUSY.
  sqlite3_busy_timeout(db, 5000);
  if (auto schema = EnsureSchema(db); !schema) {
    sqlite3_close(db);
    return schema.error();
  }
  return db;
}

Roe<void> ThreadLocalPrefsStore::EnsureSchema(sqlite3* profile_db) const {
  char* err = nullptr;
  if (sqlite3_exec(profile_db, kThreadLocalPrefsSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "thread prefs schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<std::vector<ThreadLocalPrefs>> ThreadLocalPrefsStore::List() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT thread_id, pinned_at, muted_until, archived FROM thread_local_prefs;", -1, &stmt,
                         nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare thread prefs list");
  }
  std::vector<ThreadLocalPrefs> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(PrefsFromStmt(stmt));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return out;
}

Roe<ThreadLocalPrefs> ThreadLocalPrefsStore::Get(const std::string& thread_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          *db, "SELECT thread_id, pinned_at, muted_until, archived FROM thread_local_prefs WHERE thread_id = ?;", -1,
          &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare thread prefs get");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  ThreadLocalPrefs prefs;
  prefs.thread_id = thread_id;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    prefs = PrefsFromStmt(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return prefs;
}

Roe<void> ThreadLocalPrefsStore::Set(const ThreadLocalPrefs& prefs) const {
  if (prefs.pinned_at == 0 && prefs.muted_until == 0 && !prefs.archived) {
    return Delete(prefs.thread_id);
  }
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO thread_local_prefs (thread_id, pinned_at, muted_until, archived) VALUES (?, ?, ?, ?) "
                    "ON CONFLICT(thread_id) DO UPDATE SET pinned_at=excluded.pinned_at, "
                    "muted_until=excluded.muted_until, archived=excluded.archived;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare thread prefs upsert");
  }
  sqlite3_bind_text(stmt, 1, prefs.thread_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(prefs.pinned_at));
  sqlite3_bind_int64(stmt, 3, static_cast<sqlite3_int64>(prefs.muted_until));
  sqlite3_bind_int(stmt, 4, prefs.archived ? 1 : 0);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to store thread prefs");
  }
  return {};
}

Roe<void> ThreadLocalPrefsStore::Delete(const std::string& thread_id) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "DELETE FROM thread_local_prefs WHERE thread_id = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare thread prefs delete");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);
  const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to delete thread prefs");
  }
  return {};
}

} // namespace pbr
