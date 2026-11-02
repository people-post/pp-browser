#include "lib/storage/SqlitePeerCardStore.h"

#include "common/ValueJson.h"

#include <sqlite3.h>

#include <utility>

namespace pbr {

namespace {

/** A record row's JSON, or none when it does not read (a row this build cannot read is skipped). */
std::optional<PeerCardRecord> RecordOf(sqlite3_stmt* stmt) {
  const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  if (auto json = TryParseObject(text ? text : "")) {
    if (auto record = PeerCardRecordFromJson(*json)) {
      return std::move(record.value());
    }
  }
  return std::nullopt;
}

} // namespace

SqlitePeerCardStore::SqlitePeerCardStore(std::string profile_db_path) : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> SqlitePeerCardStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for peer cards");
  }
  sqlite3_busy_timeout(db, 5000);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS peer_cards (account_id TEXT PRIMARY KEY, record_json TEXT NOT NULL, "
                     "updated_at INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS peer_card_floors (account_id TEXT PRIMARY KEY, "
                     "issued_at INTEGER NOT NULL);",
                     nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "peer_cards schema failed";
      sqlite3_free(err);
      sqlite3_close(db);
      return Error(message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<std::optional<PeerCardRecord>> SqlitePeerCardStore::Get(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT record_json FROM peer_cards WHERE account_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card read");
  }
  sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<PeerCardRecord> found;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    found = RecordOf(stmt);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return found;
}

Roe<std::vector<PeerCardRecord>> SqlitePeerCardStore::List() const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT record_json FROM peer_cards;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card list");
  }
  std::vector<PeerCardRecord> records;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    if (auto record = RecordOf(stmt)) {
      records.push_back(std::move(*record));
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return records;
}

Roe<void> SqlitePeerCardStore::Put(const PeerCardRecord& record) {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          *db,
          "INSERT INTO peer_cards (account_id, record_json, updated_at) VALUES (?, ?, strftime('%s','now')) "
          "ON CONFLICT(account_id) DO UPDATE SET record_json=excluded.record_json, "
          "updated_at=excluded.updated_at;",
          -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card write");
  }
  const std::string json = DumpJson(PeerCardRecordToJson(record));
  sqlite3_bind_text(stmt, 1, record.keys.account_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
  const bool done = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!done) {
    return Error("Failed to write peer card");
  }
  return {};
}

Roe<int64_t> SqlitePeerCardStore::Floor(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT issued_at FROM peer_card_floors WHERE account_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card floor read");
  }
  sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
  int64_t floor = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    floor = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return floor;
}

Roe<void> SqlitePeerCardStore::RaiseFloors(const std::vector<PeerCardFloor>& floors) {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "INSERT INTO peer_card_floors (account_id, issued_at) VALUES (?, ?) "
                         "ON CONFLICT(account_id) DO UPDATE SET issued_at=MAX(issued_at, excluded.issued_at);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card floor write");
  }
  bool ok = true;
  for (const PeerCardFloor& floor : floors) {
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, floor.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, floor.issued_at);
    ok = ok && sqlite3_step(stmt) == SQLITE_DONE;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to write peer card floor");
  }
  return {};
}

Roe<std::vector<PeerCardFloor>> SqlitePeerCardStore::Floors() const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT account_id, issued_at FROM peer_card_floors;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card floor list");
  }
  std::vector<PeerCardFloor> floors;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    floors.push_back({id ? id : "", sqlite3_column_int64(stmt, 1)});
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return floors;
}

} // namespace pbr
