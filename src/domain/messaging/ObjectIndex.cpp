#include "domain/messaging/ObjectIndex.h"

#include "foundation/crypto/CryptoUtil.h"
#include "common/PbrCompat.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>

namespace pbr {
namespace {

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS cas_objects (
  realm TEXT NOT NULL,
  content_id_hex TEXT NOT NULL,
  mime TEXT NOT NULL DEFAULT '',
  filename TEXT NOT NULL DEFAULT '',
  byte_length INTEGER NOT NULL DEFAULT 0,
  published_from_hex TEXT NOT NULL DEFAULT '',
  pinned INTEGER NOT NULL DEFAULT 1,
  created_at_ms INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (realm, content_id_hex)
);
)sql";

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

ObjectIndex::ObjectIndex(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)),
      db_path_((std::filesystem::path(profile_dir_) / "object_index.db").string()) {}

ObjectIndex::~ObjectIndex() {
  std::lock_guard lock(mutex_);
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

Roe<void> ObjectIndex::MigrateSchema(sqlite3* db) const {
  char* err = nullptr;
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err != nullptr ? err : "CAS object_index schema migrate failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<void> ObjectIndex::EnsureOpen() const {
  if (db_ != nullptr) {
    return {};
  }
  if (profile_dir_.empty()) {
    return Error("CAS object_index requires profile directory");
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_dir_, ec);
  if (ec) {
    return Error("Failed to create profile directory for object_index");
  }
  sqlite3* db = nullptr;
  if (sqlite3_open(db_path_.c_str(), &db) != SQLITE_OK) {
    const std::string message = db != nullptr ? sqlite3_errmsg(db) : "sqlite3_open failed";
    if (db != nullptr) {
      sqlite3_close(db);
    }
    return Error(message);
  }
  if (auto migrated = MigrateSchema(db); !migrated) {
    sqlite3_close(db);
    return migrated;
  }
  db_ = db;
  return {};
}

Roe<void> ObjectIndex::Upsert(const CasObjectMeta& meta) {
  if (meta.content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  std::lock_guard lock(mutex_);
  if (auto opened = EnsureOpen(); !opened) {
    return opened;
  }
  const std::string id_hex = BytesToHex(meta.content_id);
  constexpr const char* kSql =
      "INSERT INTO cas_objects(realm, content_id_hex, mime, filename, byte_length, published_from_hex, pinned, "
      "created_at_ms) VALUES(?,?,?,?,?,?,?,?) "
      "ON CONFLICT(realm, content_id_hex) DO UPDATE SET "
      "mime=excluded.mime, filename=excluded.filename, byte_length=excluded.byte_length, "
      "published_from_hex=excluded.published_from_hex, pinned=excluded.pinned";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(sqlite3_errmsg(db_));
  }
  const int64_t created = meta.created_at_ms > 0 ? meta.created_at_ms : NowMs();
  sqlite3_bind_text(stmt, 1, CasRealmToString(meta.realm), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, meta.mime.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, meta.filename.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, static_cast<sqlite3_int64>(meta.byte_length));
  sqlite3_bind_text(stmt, 6, meta.published_from_hex.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 7, meta.pinned ? 1 : 0);
  sqlite3_bind_int64(stmt, 8, created);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error(sqlite3_errmsg(db_));
  }
  return {};
}

Roe<std::optional<CasObjectMeta>> ObjectIndex::Lookup(const CasRealm realm, const ByteVector& content_id) const {
  if (content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  std::lock_guard lock(mutex_);
  if (auto opened = EnsureOpen(); !opened) {
    return opened.error();
  }
  const std::string id_hex = BytesToHex(content_id);
  constexpr const char* kSql =
      "SELECT mime, filename, byte_length, published_from_hex, pinned, created_at_ms FROM cas_objects "
      "WHERE realm=? AND content_id_hex=?";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(sqlite3_errmsg(db_));
  }
  sqlite3_bind_text(stmt, 1, CasRealmToString(realm), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id_hex.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return std::optional<CasObjectMeta>{};
  }
  if (rc != SQLITE_ROW) {
    const std::string message = sqlite3_errmsg(db_);
    sqlite3_finalize(stmt);
    return Error(message);
  }
  CasObjectMeta meta;
  meta.realm = realm;
  meta.content_id = content_id;
  if (const auto* text = sqlite3_column_text(stmt, 0); text != nullptr) {
    meta.mime = reinterpret_cast<const char*>(text);
  }
  if (const auto* text = sqlite3_column_text(stmt, 1); text != nullptr) {
    meta.filename = reinterpret_cast<const char*>(text);
  }
  meta.byte_length = static_cast<uint64_t>(sqlite3_column_int64(stmt, 2));
  if (const auto* text = sqlite3_column_text(stmt, 3); text != nullptr) {
    meta.published_from_hex = reinterpret_cast<const char*>(text);
  }
  meta.pinned = sqlite3_column_int(stmt, 4) != 0;
  meta.created_at_ms = sqlite3_column_int64(stmt, 5);
  sqlite3_finalize(stmt);
  return std::optional<CasObjectMeta>{std::move(meta)};
}

Roe<void> ObjectIndex::Remove(const CasRealm realm, const ByteVector& content_id) {
  if (content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  std::lock_guard lock(mutex_);
  if (auto opened = EnsureOpen(); !opened) {
    return opened;
  }
  const std::string id_hex = BytesToHex(content_id);
  constexpr const char* kSql = "DELETE FROM cas_objects WHERE realm=? AND content_id_hex=?";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(sqlite3_errmsg(db_));
  }
  sqlite3_bind_text(stmt, 1, CasRealmToString(realm), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, id_hex.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error(sqlite3_errmsg(db_));
  }
  return {};
}

Roe<void> ObjectIndex::SetPinned(const CasRealm realm, const ByteVector& content_id, const bool pinned) {
  if (content_id.size() != kCasContentIdSize) {
    return Error("Invalid CAS content id");
  }
  std::lock_guard lock(mutex_);
  if (auto opened = EnsureOpen(); !opened) {
    return opened;
  }
  const std::string id_hex = BytesToHex(content_id);
  constexpr const char* kSql = "UPDATE cas_objects SET pinned=? WHERE realm=? AND content_id_hex=?";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(sqlite3_errmsg(db_));
  }
  sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
  sqlite3_bind_text(stmt, 2, CasRealmToString(realm), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, id_hex.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error(sqlite3_errmsg(db_));
  }
  return {};
}

} // namespace pbr
