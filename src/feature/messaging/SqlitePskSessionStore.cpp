#include "feature/messaging/SqlitePskSessionStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/FileCipher.h"
#include "base/crypto/PskBundleCodec.h"
#include "base/crypto/PskFingerprint.h"
#include "foundation/error/AppError.h"

#include <sodium.h>
#include <sqlite3.h>

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string ChannelToDb(const CryptoChannel channel) {
  return CryptoChannelToString(channel);
}

CryptoChannel ChannelFromDb(const std::string& value) {
  return value == "e2e_public" ? CryptoChannel::E2ePublic : CryptoChannel::E2e;
}

std::vector<RetiredPskEntry> ParseRetiredPsks(const char* json_text) {
  std::vector<RetiredPskEntry> retired_psks;
  if (json_text == nullptr) {
    return retired_psks;
  }
  auto parsed = ParseValue(std::string(json_text));
  if (!parsed) {
    return retired_psks;
  }
  const Array* retired = asArray(*parsed);
  if (!retired) {
    return retired_psks;
  }
  for (const Value& item_value : retired->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    RetiredPskEntry entry;
    entry.epoch = static_cast<uint32_t>(item->getNonNegInt("epoch").value_or(0));
    entry.master_psk_b64 = item->getString("master_psk_b64").value_or(std::string{});
    entry.retired_at = item->getIf<int64_t>("retired_at").value_or(0);
    retired_psks.push_back(std::move(entry));
  }
  return retired_psks;
}

void ApplyFingerprint(PskSessionRecord& record) {
  record.psk_fingerprint.reset();
  if (!record.master_psk_b64) {
    return;
  }
  if (auto psk = Base64Decode(*record.master_psk_b64)) {
    if (auto fingerprint = PskFingerprint::Compute(*psk)) {
      record.psk_fingerprint = PskFingerprint::FormatDisplay(*fingerprint);
    }
  }
}

} // namespace

SqlitePskSessionStore::SqlitePskSessionStore(std::string profile_db_path, std::string profile_id)
    : profile_db_path_(std::move(profile_db_path)), profile_id_(std::move(profile_id)) {
  redirectLogger("SqlitePskSessionStore");
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

SqlitePskSessionStore::~SqlitePskSessionStore() {
  std::lock_guard lock(mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> SqlitePskSessionStore::SetDek(ByteVector dek) {
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

void SqlitePskSessionStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> SqlitePskSessionStore::RequireDek() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return AppError::Pin(Err::Pin::Required, "PSK store DEK not set (unlock profile vault first)");
  }
  return {};
}

Roe<std::string> SqlitePskSessionStore::EncryptFieldB64(const std::string& plaintext_b64,
                                                       const char* aad_kind) const {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  const ByteVector plain(plaintext_b64.begin(), plaintext_b64.end());
  const std::string aad = FileCipher::BuildAad(aad_kind, profile_id_);
  auto cipher = FileCipher::Encrypt(dek_, plain, aad);
  if (!cipher) {
    return cipher.error();
  }
  return Base64Encode(*cipher);
}

Roe<std::string> SqlitePskSessionStore::DecryptFieldB64(const std::string& ciphertext_b64,
                                                       const char* aad_kind) const {
  if (auto dek = RequireDek(); !dek) {
    return dek.error();
  }
  auto blob = Base64Decode(ciphertext_b64);
  if (!blob) {
    return blob.error();
  }
  const std::string aad = FileCipher::BuildAad(aad_kind, profile_id_);
  auto plain = FileCipher::Decrypt(dek_, *blob, aad);
  if (!plain) {
    return plain.error();
  }
  return std::string(plain->begin(), plain->end());
}

Roe<std::string> SqlitePskSessionStore::EncryptPskB64(const std::string& plaintext_b64) const {
  return EncryptFieldB64(plaintext_b64, "psk");
}

Roe<std::string> SqlitePskSessionStore::DecryptPskB64(const std::string& ciphertext_b64) const {
  return DecryptFieldB64(ciphertext_b64, "psk");
}

Roe<PskSessionRecord> SqlitePskSessionStore::DecryptRecord(PskSessionRecord record) const {
  if (record.master_psk_b64) {
    auto decrypted = DecryptPskB64(*record.master_psk_b64);
    if (!decrypted) {
      return decrypted.error();
    }
    record.master_psk_b64 = *decrypted;
  }
  for (RetiredPskEntry& entry : record.retired_psks) {
    if (entry.master_psk_b64.empty()) {
      continue;
    }
    auto decrypted = DecryptPskB64(entry.master_psk_b64);
    if (!decrypted) {
      return decrypted.error();
    }
    entry.master_psk_b64 = *decrypted;
  }
  if (record.thread_kem_sk_b64) {
    auto decrypted = DecryptFieldB64(*record.thread_kem_sk_b64, "thread_kem_sk");
    if (!decrypted) {
      return decrypted.error();
    }
    record.thread_kem_sk_b64 = *decrypted;
  }
  return record;
}

Roe<PskSessionRecord> SqlitePskSessionStore::EncryptRecord(PskSessionRecord record) const {
  if (record.master_psk_b64) {
    auto encrypted = EncryptPskB64(*record.master_psk_b64);
    if (!encrypted) {
      return encrypted.error();
    }
    record.master_psk_b64 = *encrypted;
  }
  for (RetiredPskEntry& entry : record.retired_psks) {
    if (entry.master_psk_b64.empty()) {
      continue;
    }
    auto encrypted = EncryptPskB64(entry.master_psk_b64);
    if (!encrypted) {
      return encrypted.error();
    }
    entry.master_psk_b64 = *encrypted;
  }
  if (record.thread_kem_sk_b64) {
    auto encrypted = EncryptFieldB64(*record.thread_kem_sk_b64, "thread_kem_sk");
    if (!encrypted) {
      return encrypted.error();
    }
    record.thread_kem_sk_b64 = *encrypted;
  }
  return record;
}

Roe<sqlite3*> SqlitePskSessionStore::OpenDb() const {
  std::lock_guard lock(mutex_);
  if (db_) {
    return db_;
  }
  if (sqlite3_open(profile_db_path_.c_str(), &db_) != SQLITE_OK) {
    return Error("Failed to open profile.db for PSK store");
  }
  return db_;
}

Roe<std::optional<PskSessionRecord>> SqlitePskSessionStore::Load(const ChatTargetKey& key) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT session_epoch, master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json, "
      "key_scope, thread_kem_pk_b64, thread_kem_sk_b64, peer_thread_kem_pk_b64, last_psk_rotate_at, "
      "psk_rotate_msg_count, last_rotation_id FROM chat_targets "
      "WHERE peer_identity_kind = ? AND peer_identity_value = ? AND channel = ? LIMIT 1;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare PSK load");
  }
  sqlite3_bind_text(stmt, 1, key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(key.channel).c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::optional<PskSessionRecord>{};
  }
  PskSessionRecord record;
  record.key = key;
  record.session_epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 0));
  if (sqlite3_column_text(stmt, 1)) {
    record.master_psk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  }
  if (sqlite3_column_text(stmt, 2)) {
    record.psk_fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  }
  if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
    record.psk_verified_at = sqlite3_column_int64(stmt, 3);
  }
  record.retired_psks = ParseRetiredPsks(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)));
  if (sqlite3_column_text(stmt, 5)) {
    record.key_scope = PublicKeyScopeFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5)));
  }
  if (sqlite3_column_text(stmt, 6)) {
    record.thread_kem_pk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  }
  if (sqlite3_column_text(stmt, 7)) {
    record.thread_kem_sk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
  }
  if (sqlite3_column_text(stmt, 8)) {
    record.peer_thread_kem_pk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
  }
  if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) {
    record.last_psk_rotate_at = sqlite3_column_int64(stmt, 9);
  }
  record.psk_rotate_msg_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
  if (sqlite3_column_text(stmt, 11)) {
    record.last_rotation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
  }
  sqlite3_finalize(stmt);
  if (!record.master_psk_b64 && record.retired_psks.empty() && !record.thread_kem_sk_b64) {
    return std::optional<PskSessionRecord>(record);
  }
  auto decrypted = DecryptRecord(std::move(record));
  if (!decrypted) {
    return decrypted.error();
  }
  return std::optional<PskSessionRecord>(*decrypted);
}

Roe<std::vector<PskSessionRecord>> SqlitePskSessionStore::List() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT peer_identity_kind, peer_identity_value, channel, session_epoch, master_psk_b64, psk_fingerprint, "
      "psk_verified_at, retired_psks_json, key_scope, thread_kem_pk_b64, thread_kem_sk_b64, "
      "peer_thread_kem_pk_b64, last_psk_rotate_at, psk_rotate_msg_count, last_rotation_id FROM chat_targets;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare PSK list");
  }
  std::vector<PskSessionRecord> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PskSessionRecord record;
    if (sqlite3_column_text(stmt, 0)) {
      record.key.peer_identity_kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    }
    if (sqlite3_column_text(stmt, 1)) {
      record.key.peer_identity_value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    }
    if (sqlite3_column_text(stmt, 2)) {
      record.key.channel = ChannelFromDb(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    }
    record.session_epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 3));
    if (sqlite3_column_text(stmt, 4)) {
      record.master_psk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    }
    if (sqlite3_column_text(stmt, 5)) {
      record.psk_fingerprint = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    }
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
      record.psk_verified_at = sqlite3_column_int64(stmt, 6);
    }
    record.retired_psks = ParseRetiredPsks(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
    if (sqlite3_column_text(stmt, 8)) {
      record.key_scope = PublicKeyScopeFromString(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8)));
    }
    if (sqlite3_column_text(stmt, 9)) {
      record.thread_kem_pk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    }
    if (sqlite3_column_text(stmt, 10)) {
      record.thread_kem_sk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    }
    if (sqlite3_column_text(stmt, 11)) {
      record.peer_thread_kem_pk_b64 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    }
    if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
      record.last_psk_rotate_at = sqlite3_column_int64(stmt, 12);
    }
    record.psk_rotate_msg_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 13));
    if (sqlite3_column_text(stmt, 14)) {
      record.last_rotation_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 14));
    }
    if (!record.master_psk_b64 && record.retired_psks.empty() && !record.thread_kem_sk_b64) {
      continue;
    }
    auto decrypted = DecryptRecord(std::move(record));
    if (!decrypted) {
      sqlite3_finalize(stmt);
      return decrypted.error();
    }
    rows.push_back(std::move(*decrypted));
  }
  sqlite3_finalize(stmt);
  return rows;
}

Roe<void> SqlitePskSessionStore::Save(const PskSessionRecord& record) {
  PskSessionRecord to_save = record;
  ApplyFingerprint(to_save);
  PskBundleCodec::CapRetiredTail(to_save.retired_psks, to_save.session_epoch);

  auto encrypted = EncryptRecord(std::move(to_save));
  if (!encrypted) {
    return encrypted.error();
  }
  to_save = *encrypted;

  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  std::vector<Value> retired_elements;
  retired_elements.reserve(to_save.retired_psks.size());
  for (const RetiredPskEntry& entry : to_save.retired_psks) {
    Object item;
    item.setJsonUInt("epoch", entry.epoch);
    item.set("master_psk_b64", entry.master_psk_b64);
    item.set("retired_at", entry.retired_at);
    retired_elements.push_back(ObjectValue(std::move(item)));
  }
  const std::string retired_json =
      retired_elements.empty() ? "[]" : DumpJson(ArrayValue(std::move(retired_elements)));
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, local_thread_id, session_epoch, "
      "master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json, key_scope, thread_kem_pk_b64, "
      "thread_kem_sk_b64, peer_thread_kem_pk_b64, last_psk_rotate_at, psk_rotate_msg_count, last_rotation_id) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(peer_identity_kind, peer_identity_value, channel) DO UPDATE SET "
      "session_epoch=excluded.session_epoch, master_psk_b64=excluded.master_psk_b64, "
      "psk_fingerprint=excluded.psk_fingerprint, psk_verified_at=excluded.psk_verified_at, "
      "retired_psks_json=excluded.retired_psks_json, key_scope=excluded.key_scope, "
      "thread_kem_pk_b64=excluded.thread_kem_pk_b64, thread_kem_sk_b64=excluded.thread_kem_sk_b64, "
      "peer_thread_kem_pk_b64=excluded.peer_thread_kem_pk_b64, last_psk_rotate_at=excluded.last_psk_rotate_at, "
      "psk_rotate_msg_count=excluded.psk_rotate_msg_count, last_rotation_id=excluded.last_rotation_id;";
  if (sqlite3_prepare_v2(*db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare PSK save");
  }
  sqlite3_bind_text(stmt, 1, to_save.key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, to_save.key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(to_save.key.channel).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, "pending", -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 5, static_cast<int>(to_save.session_epoch));
  if (to_save.master_psk_b64) {
    sqlite3_bind_text(stmt, 6, to_save.master_psk_b64->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 6);
  }
  if (to_save.psk_fingerprint) {
    sqlite3_bind_text(stmt, 7, to_save.psk_fingerprint->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 7);
  }
  if (to_save.psk_verified_at) {
    sqlite3_bind_int64(stmt, 8, *to_save.psk_verified_at);
  } else {
    sqlite3_bind_null(stmt, 8);
  }
  sqlite3_bind_text(stmt, 9, retired_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, PublicKeyScopeToString(to_save.key_scope), -1, SQLITE_TRANSIENT);
  if (to_save.thread_kem_pk_b64) {
    sqlite3_bind_text(stmt, 11, to_save.thread_kem_pk_b64->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 11);
  }
  if (to_save.thread_kem_sk_b64) {
    sqlite3_bind_text(stmt, 12, to_save.thread_kem_sk_b64->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 12);
  }
  if (to_save.peer_thread_kem_pk_b64) {
    sqlite3_bind_text(stmt, 13, to_save.peer_thread_kem_pk_b64->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 13);
  }
  if (to_save.last_psk_rotate_at) {
    sqlite3_bind_int64(stmt, 14, *to_save.last_psk_rotate_at);
  } else {
    sqlite3_bind_null(stmt, 14);
  }
  sqlite3_bind_int(stmt, 15, static_cast<int>(to_save.psk_rotate_msg_count));
  if (to_save.last_rotation_id) {
    sqlite3_bind_text(stmt, 16, to_save.last_rotation_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 16);
  }
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return Error("Failed to save PSK row");
  }
  sqlite3_finalize(stmt);
  return {};
}

Roe<ByteVector> SqlitePskSessionStore::GenerateMasterPsk() {
  EnsureSodiumInit();
  ByteVector psk(kMasterPskSize);
  randombytes_buf(psk.data(), psk.size());
  return psk;
}

Roe<std::optional<std::string>> SqlitePskSessionStore::ResolveMasterPskForEpoch(const ChatTargetKey& key,
                                                                              const uint32_t envelope_epoch) const {
  auto loaded = Load(key);
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->has_value()) {
    return std::optional<std::string>{};
  }
  const PskSessionRecord& record = **loaded;
  if (envelope_epoch == record.session_epoch && record.master_psk_b64) {
    return std::optional<std::string>(*record.master_psk_b64);
  }
  for (const RetiredPskEntry& retired : record.retired_psks) {
    if (retired.epoch == envelope_epoch) {
      return std::optional<std::string>(retired.master_psk_b64);
    }
  }
  return std::optional<std::string>{};
}

Roe<void> SqlitePskSessionStore::MarkPskVerified(const ChatTargetKey& key, const int64_t verified_at_ms) {
  auto loaded = Load(key);
  if (!loaded || !loaded->has_value()) {
    return Error("PSK session not found");
  }
  PskSessionRecord record = **loaded;
  record.psk_verified_at = verified_at_ms;
  return Save(record);
}

Roe<bool> SqlitePskSessionStore::IsPskVerified(const ChatTargetKey& key) const {
  auto loaded = Load(key);
  if (!loaded) {
    return loaded.error();
  }
  return loaded->has_value() && loaded->value().psk_verified_at.has_value();
}

Roe<PskBundleV1> SqlitePskSessionStore::ExportPskBundle(const ChatTargetKey& key) const {
  auto loaded = Load(key);
  if (!loaded || !loaded->has_value() || !loaded->value().master_psk_b64) {
    return Error("No PSK to export");
  }
  const PskSessionRecord& record = loaded->value();
  PskBundleV1 bundle;
  bundle.channel = key.channel;
  bundle.active_epoch = record.session_epoch;
  bundle.master_psk_b64 = *record.master_psk_b64;
  bundle.retired_epochs = record.retired_psks;
  PskBundleCodec::CapRetiredTail(bundle.retired_epochs, bundle.active_epoch);
  if (auto valid = PskBundleCodec::ValidateBundle(bundle); !valid) {
    return valid.error();
  }
  return bundle;
}

Roe<void> SqlitePskSessionStore::ImportPskBundle(const ChatTargetKey& key, const PskBundleV1& bundle) {
  if (bundle.channel != key.channel) {
    return Error("PSK bundle channel mismatch");
  }
  if (auto valid = PskBundleCodec::ValidateBundle(bundle); !valid) {
    return valid.error();
  }

  PskSessionRecord record;
  record.key = key;
  record.session_epoch = bundle.active_epoch;
  record.master_psk_b64 = bundle.master_psk_b64;
  record.psk_verified_at = std::nullopt;
  if (auto existing = Load(key); existing && existing->has_value()) {
    record.retired_psks = PskBundleCodec::MergeRetired(existing->value().retired_psks, bundle.retired_epochs);
    record.key_scope = existing->value().key_scope;
    record.thread_kem_pk_b64 = existing->value().thread_kem_pk_b64;
    record.thread_kem_sk_b64 = existing->value().thread_kem_sk_b64;
    record.peer_thread_kem_pk_b64 = existing->value().peer_thread_kem_pk_b64;
    record.last_psk_rotate_at = existing->value().last_psk_rotate_at;
    record.psk_rotate_msg_count = existing->value().psk_rotate_msg_count;
    record.last_rotation_id = existing->value().last_rotation_id;
  } else {
    record.retired_psks = bundle.retired_epochs;
  }
  PskBundleCodec::CapRetiredTail(record.retired_psks, record.session_epoch);
  return Save(record);
}

} // namespace pbr
