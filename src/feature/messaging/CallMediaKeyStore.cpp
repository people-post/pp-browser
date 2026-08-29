#include "feature/messaging/CallMediaKeyStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/EncryptedPayload.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/MessageCipher.h"
#include "base/error/AppError.h"
#include "base/messaging/CallSessionStore.h"
#include "common/Utilities.h"

#include <sodium.h>
#include <sqlite3.h>
#include "common/PbrCompat.h"

namespace pbr {

CallMediaKeyStore::CallMediaKeyStore(std::string profile_db_path, std::string profile_id)
    : profile_db_path_(std::move(profile_db_path)), profile_id_(std::move(profile_id)) {
  redirectLogger("CallMediaKeyStore");
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

Roe<void> CallMediaKeyStore::SetDek(ByteVector dek) {
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

void CallMediaKeyStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> CallMediaKeyStore::RequireDek() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return AppError::Pin(Err::Pin::Required, "Call media key store DEK not set (unlock profile vault first)");
  }
  return {};
}

Roe<sqlite3*> CallMediaKeyStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    return Error("Failed to open profile.db for call media keys");
  }
  CallSessionStore schema(profile_db_path_);
  if (auto ensured = schema.EnsureSchema(db); !ensured) {
    sqlite3_close(db);
    return ensured.error();
  }
  return db;
}

Roe<ByteVector> CallMediaKeyStore::GenerateEpochKey() const {
  EnsureSodiumInit();
  ByteVector key(32);
  randombytes_buf(key.data(), key.size());
  return key;
}

std::string CallMediaKeyStore::BuildWrapAad(const std::string& call_id, const uint32_t media_epoch,
                                            const std::string& media_key_id) {
  return "call_media_key|" + call_id + "|" + std::to_string(media_epoch) + "|" + media_key_id;
}

Roe<std::string> CallMediaKeyStore::WrapKeyB64(const ByteVector& session_key, const ByteVector& key_bytes,
                                               const std::string& call_id, const uint32_t media_epoch,
                                               const std::string& media_key_id) {
  if (session_key.empty() || key_bytes.empty() || call_id.empty() || media_key_id.empty()) {
    return Error("WrapKeyB64 requires session_key, key_bytes, call_id, media_key_id");
  }
  auto nonce = MessageCipher::GenerateNonce();
  if (!nonce) {
    return nonce.error();
  }
  const std::string aad = BuildWrapAad(call_id, media_epoch, media_key_id);
  const ByteVector aad_bytes(aad.begin(), aad.end());
  auto encrypted = MessageCipher::Encrypt(session_key, key_bytes, aad_bytes, *nonce);
  if (!encrypted) {
    return encrypted.error();
  }
  auto blob = EncryptedPayload::EncodeBlob(*encrypted);
  if (!blob) {
    return blob.error();
  }
  return EncryptedPayload::EncodeBase64(*blob);
}

Roe<ByteVector> CallMediaKeyStore::UnwrapKeyB64(const ByteVector& session_key, const std::string& wrapped_key_b64,
                                                const std::string& call_id, const uint32_t media_epoch,
                                                const std::string& media_key_id) {
  if (session_key.empty() || wrapped_key_b64.empty() || call_id.empty() || media_key_id.empty()) {
    return Error("UnwrapKeyB64 requires session_key, wrapped_key_b64, call_id, media_key_id");
  }
  auto blob_bytes = EncryptedPayload::DecodeBase64(wrapped_key_b64);
  if (!blob_bytes) {
    return blob_bytes.error();
  }
  auto blob = EncryptedPayload::DecodeBlob(*blob_bytes);
  if (!blob) {
    return blob.error();
  }
  const std::string aad = BuildWrapAad(call_id, media_epoch, media_key_id);
  const ByteVector aad_bytes(aad.begin(), aad.end());
  return MessageCipher::Decrypt(session_key, *blob, aad_bytes);
}

Roe<std::string> CallMediaKeyStore::PutEpochKey(const std::string& call_id, const uint32_t media_epoch,
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

Roe<std::optional<ByteVector>> CallMediaKeyStore::LoadEpochKey(const std::string& call_id,
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
