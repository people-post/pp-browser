#include "domain/messaging/ControlLaneSeenStore.h"

#include <sqlite3.h>

#include <memory>
#include <string>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS control_lane_seen (
  sender TEXT NOT NULL,
  control_id TEXT NOT NULL,
  seen_at_ms INTEGER NOT NULL,
  PRIMARY KEY (sender, control_id)
);
CREATE INDEX IF NOT EXISTS control_lane_seen_at ON control_lane_seen (seen_at_ms);
)SQL";

/** Prune at most this often (inserts are cheap; the table stays small). */
constexpr int64_t kPruneEveryMs = 5 * 60 * 1000;

struct DbCloser {
  void operator()(sqlite3* db) const { sqlite3_close(db); }
};

} // namespace

ControlLaneSeenStore::ControlLaneSeenStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> ControlLaneSeenStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for the control lane");
  }
  sqlite3_busy_timeout(db, 5000);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(db, kSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "control_lane_seen schema failed";
      sqlite3_free(err);
      sqlite3_close(db);
      return Error(message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<bool> ControlLaneSeenStore::FirstSight(const std::string& sender, const std::string& control_id,
                                           const int64_t now_ms) {
  if (sender.empty() || control_id.empty()) {
    return Error("control lane message without sender or id");
  }
  std::lock_guard lock(mutex_);
  auto opened = OpenDb();
  if (!opened) {
    return opened.error();
  }
  std::unique_ptr<sqlite3, DbCloser> db(*opened);

  if (now_ms - last_prune_ms_ >= kPruneEveryMs) {
    sqlite3_stmt* prune = nullptr;
    if (sqlite3_prepare_v2(db.get(), "DELETE FROM control_lane_seen WHERE seen_at_ms < ?1", -1, &prune, nullptr) ==
        SQLITE_OK) {
      sqlite3_bind_int64(prune, 1, now_ms - kRetentionMs);
      (void)sqlite3_step(prune);
    }
    sqlite3_finalize(prune);
    last_prune_ms_ = now_ms;
  }

  sqlite3_stmt* insert = nullptr;
  if (sqlite3_prepare_v2(db.get(),
                         "INSERT OR IGNORE INTO control_lane_seen (sender, control_id, seen_at_ms) VALUES (?1, ?2, ?3)",
                         -1, &insert, nullptr) != SQLITE_OK) {
    sqlite3_finalize(insert);
    return Error("control_lane_seen insert prepare failed");
  }
  sqlite3_bind_text(insert, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(insert, 2, control_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(insert, 3, now_ms);
  const int rc = sqlite3_step(insert);
  sqlite3_finalize(insert);
  if (rc != SQLITE_DONE) {
    return Error("control_lane_seen insert failed");
  }
  return sqlite3_changes(db.get()) == 1;
}

void ControlLaneSeenStore::Forget(const std::string& sender, const std::string& control_id) {
  std::lock_guard lock(mutex_);
  auto opened = OpenDb();
  if (!opened) {
    return;
  }
  std::unique_ptr<sqlite3, DbCloser> db(*opened);
  sqlite3_stmt* remove = nullptr;
  if (sqlite3_prepare_v2(db.get(), "DELETE FROM control_lane_seen WHERE sender = ?1 AND control_id = ?2", -1, &remove,
                         nullptr) == SQLITE_OK) {
    sqlite3_bind_text(remove, 1, sender.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(remove, 2, control_id.c_str(), -1, SQLITE_TRANSIENT);
    (void)sqlite3_step(remove);
  }
  sqlite3_finalize(remove);
}

} // namespace pbr
