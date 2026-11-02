#include "lib/storage/SqliteCallMediaKeyStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/EncryptedPayload.h"
#include "foundation/crypto/FileCipher.h"
#include "foundation/crypto/MessageCipher.h"
#include "lib/calls/MediaKeyWrap.h"
#include "foundation/error/AppError.h"
#include "lib/storage/SqliteCallSessionStore.h"
#include "common/Utilities.h"

#include <sodium.h>
#include <sqlite3.h>
#include "common/PbrCompat.h"

namespace pbr {

SqliteCallMediaKeyStore::SqliteCallMediaKeyStore(std::string profile_db_path, std::string profile_id)
    : profile_db_path_(std::move(profile_db_path)), profile_id_(std::move(profile_id)) {
  redirectLogger("SqliteCallMediaKeyStore");
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

Roe<void> SqliteCallMediaKeyStore::SetDek(ByteVector dek) {
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

void SqliteCallMediaKeyStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> SqliteCallMediaKeyStore::RequireDek() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return AppError::Pin(Err::Pin::Required, "Call media key store DEK not set (unlock profile vault first)");
  }
  return {};
}

Roe<sqlite3*> SqliteCallMediaKeyStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for call media keys");
  }
  sqlite3_busy_timeout(db, 5000);
  std::lock_guard<std::mutex> lock(schema_mutex_);
  if (!schema_ready_) {
    if (auto ensured = SqliteCallSessionStore::EnsureSchema(db); !ensured) {
      sqlite3_close(db);
      return ensured.error();
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<std::string> SqliteCallMediaKeyStore::PutEpochKey(const std::string& call_id, const uint32_t media_epoch,
                                               const ByteVector& key_bytes) {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  if (call_id.empty() || key_bytes.empty()) {
    return Error("call_id and key_bytes required");
  }
  const std::string aad = FileCipher::BuildAad("call_media", profile_id_);
  auto cipher = FileCipher::Encrypt(dek_, key_bytes, aad);
  if (!cipher) {
    return cipher.error();
  }
  const std::string media_key_id = "mk:" + util::GenerateUuid();
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO call_media_keys (call_id, media_epoch, media_key_id, ciphertext) VALUES (?, ?, ?, ?) "
      "ON CONFLICT(call_id, media_epoch) DO UPDATE SET media_key_id=excluded.media_key_id, "
      "ciphertext=excluded.ciphertext;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call media key upsert");
  }
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(media_epoch));
  sqlite3_bind_text(stmt, 3, media_key_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 4, cipher->data(), static_cast<int>(cipher->size()), SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    sqlite3_close(*db);
    return Error("Failed to upsert call media key");
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return media_key_id;
}

Roe<std::optional<ByteVector>> SqliteCallMediaKeyStore::LoadEpochKey(const std::string& call_id,
                                                              const uint32_t media_epoch) const {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT ciphertext FROM call_media_keys WHERE call_id = ? AND media_epoch = ? LIMIT 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare call media key load");
  }
  sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(media_epoch));
  std::optional<ByteVector> result;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const void* blob = sqlite3_column_blob(stmt, 0);
    const int size = sqlite3_column_bytes(stmt, 0);
    if (blob && size > 0) {
      const ByteVector cipher(static_cast<const uint8_t*>(blob), static_cast<const uint8_t*>(blob) + size);
      const std::string aad = FileCipher::BuildAad("call_media", profile_id_);
      auto plain = FileCipher::Decrypt(dek_, cipher, aad);
      if (!plain) {
        sqlite3_finalize(stmt);
        sqlite3_close(*db);
        return plain.error();
      }
      result = std::move(*plain);
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return result;
}

} // namespace pbr
