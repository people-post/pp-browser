#include "lib/storage/SqliteThreadDb.h"

#include "lib/storage/SqliteChatDbLadders.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteMigrations.h"
#include "lib/storage/SqliteThreadSchema.h"
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
  chat_store_gate::Hold(ProfileDbPath());
}

SqliteThreadDb::~SqliteThreadDb() {
  chat_store_gate::Release(ProfileDbPath());
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
  {
    std::lock_guard lock(dek_mutex_);
    if (!dek_.empty()) {
      sodium_memzero(dek_.data(), dek_.size());
    }
    dek_ = std::move(dek);
  }
  // The key is in: the chat store opens now, its ladder with it. A store that cannot be opened (newer than this
  // build, or a failed upgrade) does not fail the unlock — it stays shut, and says why (chat_store_gate::OpenFailure).
  if (auto opened = EnsureInitialized(); !opened) {
    log().error << "Chat store not opened after unlock: " << opened.error().message;
  }
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
  return EnsureInitialized();
}

Roe<void> SqliteThreadDb::EnsureInitialized() const {
  std::lock_guard lock(init_mutex_);
  if (initialized_) {
    return {};
  }
  if (open_failure_) {
    return *open_failure_;
  }
  // Nothing is opened, created or migrated before the profile is unlocked (D105).
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  std::error_code ec;
  std::filesystem::create_directories(SqliteThreadsRoot(data_dir_), ec);
  if (auto open = OpenProfileDb(); !open) {
    return open.error();
  }
  if (auto repair = RepairOrphanThreadDirs(); !repair) {
    return repair.error();
  }
  initialized_ = true;
  chat_store_gate::MarkOpen(ProfileDbPath());
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

Error SqliteThreadDb::RecordOpenFailure(const ChatStoreProblem problem, std::string message) const {
  log().error << message;
  Error error = AppError::Storage(Err::Storage::Unavailable, message);
  open_failure_ = error;
  chat_store_gate::MarkFailed(ProfileDbPath(), ChatStoreOpenFailure{problem, std::move(message)});
  return error;
}

Roe<void> SqliteThreadDb::OpenProfileDbUnguarded() const {
  const std::string path = SqliteThreadProfileDbFile(data_dir_);
  if (sqlite3_open(path.c_str(), &profile_db_) != SQLITE_OK) {
    return Error("Failed to open profile.db");
  }
  if (auto cfg = SqliteThreadConfigureDb(profile_db_); !cfg) {
    return cfg.error();
  }
  const ByteVector dek = CopyDek();
  auto outcome = RunSqliteLadder(profile_db_, path, ProfileDbLadder(), &dek);
  if (const auto problem = ChatStoreProblemFor(outcome)) {
    return RecordOpenFailure(*problem, *problem == ChatStoreProblem::Newer
                                           ? "Incompatible profile.db: it is from a newer version of the app "
                                             "(newer than schema " +
                                                 std::to_string(kSqliteThreadProfileUserVersion) + ")"
                                           : "Your chats could not be upgraded: " + outcome.error().message);
  }
  if (*outcome == SqliteLadderOutcome::Migrated) {
    log().info << "profile.db migrated to schema " << kSqliteThreadProfileUserVersion;
  }
  if (*outcome != SqliteLadderOutcome::BelowFloor) {
    return {};
  }
  // Older than the floor: too old to upgrade. The whole chat store goes (its thread.db files belong to this
  // catalog), as Me → Storage → Reset chats does; the account, contacts and settings live beside it.
  sqlite3_close(profile_db_);
  profile_db_ = nullptr;
  log().error << "profile.db is from a version too old to upgrade (below schema " << kSqliteThreadProfileFloorVersion
              << "): the chats were reset";
  std::error_code ec;
  std::filesystem::remove_all(SqliteThreadsRoot(data_dir_), ec);
  if (ec) {
    return Error("profile.db too old to upgrade, and the chats could not be reset: " + ec.message());
  }
  std::filesystem::create_directories(SqliteThreadsRoot(data_dir_), ec);
  NoteChatStoreReset(data_dir_, "profile.db");
  return OpenProfileDbUnguarded(); // a new, empty file: created at the current schema
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
    auto opened = OpenThreadDbFile(thread_id);
    if (!opened) {
      return opened.error();
    }
    ThreadDbHandle handle;
    handle.db = *opened;
    thread_dbs_[thread_id] = handle;
    it = thread_dbs_.find(thread_id);
  }
  TouchThreadLru(thread_id);
  EvictThreadDbsIfNeeded();
  return it->second.db;
}

Roe<sqlite3*> SqliteThreadDb::OpenThreadDbFile(const std::string& thread_id) const {
  const std::string path = SqliteThreadDbFile(data_dir_, thread_id);
  sqlite3* db = nullptr;
  if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
    const std::string detail = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db) {
      sqlite3_close(db);
    }
    return Error("Failed to open thread.db: " + thread_id + " (" + detail + ")");
  }
  if (auto cfg = SqliteThreadConfigureDb(db); !cfg) {
    sqlite3_close(db);
    return cfg.error();
  }
  const ByteVector dek = CopyDek();
  auto outcome = RunSqliteLadder(db, path, ThreadDbLadder(), &dek);
  if (!outcome || *outcome == SqliteLadderOutcome::Newer) {
    sqlite3_close(db);
    return Error((outcome ? std::string("Incompatible thread.db: it is from a newer version of the app")
                          : outcome.error().message) +
                 " (thread " + thread_id + ")");
  }
  if (*outcome == SqliteLadderOutcome::Migrated) {
    log().info << "thread.db of " << thread_id << " migrated to schema " << kSqliteThreadUserVersion;
  }
  if (*outcome != SqliteLadderOutcome::BelowFloor) {
    return db;
  }
  // Older than the floor: that chat's messages are reset; its place in the catalog stays.
  sqlite3_close(db);
  log().error << "thread.db of " << thread_id << " is from a version too old to upgrade (below schema "
              << kSqliteThreadFloorVersion << "): its messages were reset";
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    std::filesystem::remove(path + suffix, ec);
  }
  NoteChatStoreReset(data_dir_, "thread.db " + thread_id);
  return OpenThreadDbFile(thread_id); // a new, empty file: created at the current schema
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
    if (sqlite3_prepare_v2(profile_db_, "SELECT 1 FROM threads WHERE id = ? LIMIT 1;", -1, &stmt, nullptr) !=
        SQLITE_OK) {
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
