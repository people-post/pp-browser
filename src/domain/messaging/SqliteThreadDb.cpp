#include "domain/messaging/SqliteThreadDb.h"

#include "domain/messaging/SqliteThreadSchema.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "foundation/error/AppError.h"
#include "common/chat/MessagingLimits.h"
#include "common/Utilities.h"

#include <sqlite3.h>
#include <sodium.h>

#include <filesystem>

#include "common/PbrCompat.h"

namespace pbr {

SqliteThreadDb::SqliteThreadDb(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("SqliteThreadDb");
  profile_id_ = std::filesystem::path(data_dir_).filename().string();
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

SqliteThreadDb::~SqliteThreadDb() {
  ClearDek();
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

Roe<void> SqliteThreadDb::SetDek(ByteVector dek) {
  if (dek.size() != 32u) {
    return Error("Invalid DEK size");
  }
  std::lock_guard lock(dek_mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
  }
  dek_ = std::move(dek);
  return {};
}

void SqliteThreadDb::ClearDek() {
  std::lock_guard lock(dek_mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> SqliteThreadDb::RequireDek() const {
  std::lock_guard lock(dek_mutex_);
  if (dek_.size() != 32u) {
    return AppError::Pin(Err::Pin::Required, "Transcript store DEK not set (unlock profile vault first)");
  }
  return {};
}


ByteVector SqliteThreadDb::CopyDek() const {
  std::lock_guard lock(dek_mutex_);
  return dek_;
}

std::string SqliteThreadDb::ProfileDbPath() const {
  return SqliteThreadProfileDbFile(data_dir_);
}

Roe<void> SqliteThreadDb::EnsureProfileOpen() const {
  return OpenProfileDb();
}


Roe<void> SqliteThreadDb::EnsureInitialized() const {
  if (initialized_) {
    return {};
  }
  std::error_code ec;
  std::filesystem::create_directories(SqliteThreadsRoot(data_dir_), ec);
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

Roe<void> SqliteThreadDb::WipeLegacyJsonIfPresent() const {
  const std::filesystem::path index = std::filesystem::path(SqliteThreadsRoot(data_dir_)) / "index.json";
  if (!std::filesystem::exists(index)) {
    return {};
  }
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(SqliteThreadsRoot(data_dir_), ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (entry.path().extension() == ".json") {
      std::filesystem::remove(entry.path(), ec);
    }
  }
  return {};
}

Roe<void> SqliteThreadDb::OpenProfileDb() const {
  if (profile_db_) {
    return {};
  }
  auto opened = OpenProfileDbUnguarded();
  if (!opened && profile_db_) {
    // sqlite3_open() hands back a handle even when it fails, and every later
    // step leaves it assigned too. Leaving it set would make the next call take
    // the early return above and skip the schema-version check entirely, so the
    // store would go on to read an incompatible database. profile_db_ stays
    // non-null only once the database is fully usable.
    sqlite3_close(profile_db_);
    profile_db_ = nullptr;
  }
  return opened;
}

Roe<void> SqliteThreadDb::OpenProfileDbUnguarded() const {
  const bool created = !std::filesystem::exists(SqliteThreadProfileDbFile(data_dir_));
  if (sqlite3_open(SqliteThreadProfileDbFile(data_dir_).c_str(), &profile_db_) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  if (auto cfg = SqliteThreadConfigureDb(profile_db_); !cfg) {
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
    if (auto schema = SqliteThreadExecSql(profile_db_, kSqliteThreadProfileSchemaV1); !schema) {
      return schema.error();
    }
    if (auto version = SqliteThreadApplyUserVersion(profile_db_, kSqliteThreadProfileUserVersion); !version) {
      return version.error();
    }
  } else if (user_version != kSqliteThreadProfileUserVersion) {
    return Error("Incompatible profile.db — wipe profile data directory");
  }
  GroupRosterStore roster_store(SqliteThreadProfileDbFile(data_dir_));
  if (auto group_schema = roster_store.EnsureSchema(profile_db_); !group_schema) {
    return group_schema.error();
  }
  CallSessionStore call_store(SqliteThreadProfileDbFile(data_dir_));
  if (auto call_schema = call_store.EnsureSchema(profile_db_); !call_schema) {
    return call_schema.error();
  }
  return {};
}

Roe<void> SqliteThreadDb::EnsureThreadDirectory(const std::string& thread_id) const {
  std::error_code ec;
  std::filesystem::create_directories(SqliteThreadDir(data_dir_, thread_id), ec);
  if (ec) {
    return Error("Failed to create thread directory: " + thread_id + " (" + ec.message() + ")");
  }
  std::filesystem::create_directories(std::filesystem::path(SqliteThreadDir(data_dir_, thread_id)) / "blobs", ec);
  if (ec) {
    return Error("Failed to create thread blobs directory: " + thread_id + " (" + ec.message() + ")");
  }
  return {};
}

Roe<sqlite3*> SqliteThreadDb::OpenThreadDb(const std::string& thread_id) const {
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
    if (sqlite3_open(SqliteThreadDbFile(data_dir_, thread_id).c_str(), &handle.db) != SQLITE_OK) {
      const std::string detail = handle.db ? sqlite3_errmsg(handle.db) : "sqlite3_open failed";
      if (handle.db) {
        sqlite3_close(handle.db);
        handle.db = nullptr;
      }
      return Error("Failed to open thread.db: " + thread_id + " (" + detail + ")");
    }
    if (auto cfg = SqliteThreadConfigureDb(handle.db); !cfg) {
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
      if (auto schema = SqliteThreadExecSql(handle.db, kSqliteThreadSchemaV2); !schema) {
        sqlite3_close(handle.db);
        return schema.error();
      }
      if (auto version = SqliteThreadApplyUserVersion(handle.db, kSqliteThreadUserVersion); !version) {
        sqlite3_close(handle.db);
        return version.error();
      }
    } else if (user_version != kSqliteThreadUserVersion) {
      sqlite3_close(handle.db);
      return Error("Incompatible thread.db — wipe profile data directory");
    }
    thread_dbs_[thread_id] = handle;
    it = thread_dbs_.find(thread_id);
  }
  TouchThreadLru(thread_id);
  EvictThreadDbsIfNeeded();
  return it->second.db;
}

void SqliteThreadDb::TouchThreadLru(const std::string& thread_id) const {
  thread_lru_.remove(thread_id);
  thread_lru_.push_front(thread_id);
}

void SqliteThreadDb::EvictThreadDbsIfNeeded() const {
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
Roe<void> SqliteThreadDb::RepairOrphanThreadDirs() const {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(SqliteThreadsRoot(data_dir_), ec)) {
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
                           "INSERT INTO threads (id, kind, channel, title, participant_contact_ids, preview_enc, "
                           "updated_at, unread_count) VALUES (?, 'ai', '', ?, '[]', NULL, ?, 0);",
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

void SqliteThreadDb::Flush() const {
  std::lock_guard profile_lock(profile_mutex_);
  if (profile_db_) {
    (void)SqliteThreadExecSql(profile_db_, "PRAGMA wal_checkpoint(PASSIVE);");
  }
  std::lock_guard thread_lock(thread_cache_mutex_);
  for (auto& [thread_id, handle] : thread_dbs_) {
    (void)thread_id;
    if (handle.db) {
      (void)SqliteThreadExecSql(handle.db, "PRAGMA wal_checkpoint(PASSIVE);");
    }
  }
}
void SqliteThreadDb::CloseThreadDb(const std::string& thread_id) const {
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

} // namespace pbr
