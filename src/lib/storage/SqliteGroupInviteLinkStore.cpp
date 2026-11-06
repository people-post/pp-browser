#include "lib/storage/SqliteGroupInviteLinkStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/error/AppError.h"

#include <sodium.h>
#include <sqlite3.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr const char kSecretAadPurpose[] = "group_link_secret";

constexpr const char kSchema[] =
    "CREATE TABLE IF NOT EXISTS group_invite_links ("
    " link_id TEXT PRIMARY KEY,"
    " group_id TEXT NOT NULL,"
    " secret_enc BLOB NOT NULL,"
    " maker TEXT NOT NULL DEFAULT '',"
    " expires_at INTEGER NOT NULL,"
    " max_uses INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS group_invite_links_group ON group_invite_links (group_id);";

constexpr const char kColumns[] = "link_id, group_id, secret_enc, maker, expires_at, max_uses, created_at";

std::string Text(sqlite3_stmt* stmt, const int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char*>(text) : std::string();
}

} // namespace

SqliteGroupInviteLinkStore::SqliteGroupInviteLinkStore(std::string profile_db_path, std::string profile_id)
    : profile_db_path_(std::move(profile_db_path)), profile_id_(std::move(profile_id)) {
  redirectLogger("SqliteGroupInviteLinkStore");
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

Roe<void> SqliteGroupInviteLinkStore::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
  }
  dek_ = std::move(dek);
  return {};
}

void SqliteGroupInviteLinkStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<sqlite3*> SqliteGroupInviteLinkStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for invite links");
  }
  sqlite3_busy_timeout(db, 5000);
  std::lock_guard lock(schema_mutex_);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "unknown";
      sqlite3_free(err);
      sqlite3_close(db);
      return Error("Failed to create group_invite_links: " + message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<void> SqliteGroupInviteLinkStore::Keep(const KeptInviteLink& link) const {
  if (link.link_id.empty() || link.group_id.empty() || link.secret.empty()) {
    return Error("An invite link to keep has its id, group and secret");
  }
  ByteVector cipher;
  {
    std::lock_guard lock(mutex_);
    if (dek_.size() != kDataEncryptionKeySize) {
      return AppError::Pin(Err::Pin::Required, "Invite link store DEK not set (unlock profile vault first)");
    }
    auto sealed = FileCipher::Encrypt(dek_, link.secret, FileCipher::BuildAad(kSecretAadPurpose, profile_id_));
    if (!sealed) {
      return sealed.error();
    }
    cipher = std::move(*sealed);
  }
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO group_invite_links (link_id, group_id, secret_enc, maker, expires_at, max_uses, created_at) "
      "VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT(link_id) DO UPDATE SET group_id=excluded.group_id, "
      "secret_enc=excluded.secret_enc, maker=excluded.maker, expires_at=excluded.expires_at, "
      "max_uses=excluded.max_uses, created_at=excluded.created_at;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare invite link upsert");
  }
  sqlite3_bind_text(stmt, 1, link.link_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, link.group_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 3, cipher.data(), static_cast<int>(cipher.size()), SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, link.maker.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, link.expires_at);
  sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(link.max_uses));
  sqlite3_bind_int64(stmt, 7, link.created_at);
  const bool done = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!done) {
    return Error("Failed to keep invite link");
  }
  return {};
}

Roe<std::vector<KeptInviteLink>> SqliteGroupInviteLinkStore::Select(const char* sql, const std::string& param) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare invite link query");
  }
  sqlite3_bind_text(stmt, 1, param.c_str(), -1, SQLITE_TRANSIENT);
  std::vector<KeptInviteLink> rows;
  std::lock_guard lock(mutex_);
  const bool unlocked = dek_.size() == kDataEncryptionKeySize;
  const std::string aad = FileCipher::BuildAad(kSecretAadPurpose, profile_id_);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    KeptInviteLink link{.link_id = Text(stmt, 0),
                        .group_id = Text(stmt, 1),
                        .maker = Text(stmt, 3),
                        .expires_at = sqlite3_column_int64(stmt, 4),
                        .max_uses = static_cast<uint64_t>(sqlite3_column_int64(stmt, 5)),
                        .created_at = sqlite3_column_int64(stmt, 6)};
    const auto* blob = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 2));
    const int size = sqlite3_column_bytes(stmt, 2);
    if (unlocked && blob && size > 0) {
      if (auto plain = FileCipher::Decrypt(dek_, ByteVector(blob, blob + size), aad)) {
        link.secret = std::move(*plain);
      }
    }
    rows.push_back(std::move(link));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return rows;
}

Roe<void> SqliteGroupInviteLinkStore::Run(const char* sql, const std::string& text_param,
                                          const std::optional<int64_t> int_param) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare invite link update");
  }
  if (int_param) {
    sqlite3_bind_int64(stmt, 1, *int_param);
  } else {
    sqlite3_bind_text(stmt, 1, text_param.c_str(), -1, SQLITE_TRANSIENT);
  }
  const bool done = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!done) {
    return Error("Failed to update invite links");
  }
  return {};
}

Roe<std::optional<KeptInviteLink>> SqliteGroupInviteLinkStore::Find(const std::string& link_id) const {
  const std::string sql = std::string("SELECT ") + kColumns + " FROM group_invite_links WHERE link_id = ? LIMIT 1;";
  auto rows = Select(sql.c_str(), link_id);
  if (!rows) {
    return rows.error();
  }
  if (rows->empty()) {
    return std::optional<KeptInviteLink>();
  }
  return std::optional<KeptInviteLink>(std::move(rows->front()));
}

Roe<std::vector<KeptInviteLink>> SqliteGroupInviteLinkStore::ListForGroup(const std::string& group_id) const {
  const std::string sql = std::string("SELECT ") + kColumns +
                          " FROM group_invite_links WHERE group_id = ? ORDER BY created_at DESC, link_id;";
  return Select(sql.c_str(), group_id);
}

Roe<void> SqliteGroupInviteLinkStore::Forget(const std::string& link_id) const {
  return Run("DELETE FROM group_invite_links WHERE link_id = ?;", link_id, std::nullopt);
}

Roe<void> SqliteGroupInviteLinkStore::ForgetExpiredBefore(const int64_t before_ms) const {
  return Run("DELETE FROM group_invite_links WHERE expires_at < ?;", {}, before_ms);
}

} // namespace pbr
