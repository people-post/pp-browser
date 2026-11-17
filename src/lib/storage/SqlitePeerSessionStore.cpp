#include "lib/storage/SqlitePeerSessionStore.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteMigrations.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/FileCipher.h"
#include "lib/delivery/PskBundleCodec.h"
#include "lib/delivery/PskFingerprint.h"
#include "foundation/error/AppError.h"

#include <sodium.h>
#include <sqlite3.h>

#include <functional>
#include <string_view>

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

void ApplyFingerprint(PeerSessionRecord& record) {
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

constexpr const char* kDeviceSessionsSchema = R"sql(
CREATE TABLE IF NOT EXISTS device_sessions (
  peer_identity_kind TEXT NOT NULL,
  peer_identity_value TEXT NOT NULL,
  channel TEXT NOT NULL,
  peer_device TEXT NOT NULL,
  session_epoch INTEGER NOT NULL DEFAULT 1,
  next_outgoing_seq INTEGER NOT NULL DEFAULT 1,
  master_psk_b64 TEXT,
  psk_fingerprint TEXT,
  psk_verified_at INTEGER,
  retired_psks_json TEXT,
  auto_key_session_id TEXT,
  auto_key_init_pending TEXT,
  auto_key_peer_heard_ms INTEGER,
  restart_pending INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (peer_identity_kind, peer_identity_value, channel, peer_device)
);
)sql";

constexpr const char* kAccountSessionsTable = "chat_targets";
constexpr const char* kDeviceSessionsTable = "device_sessions";

/**
 * A session column, and what a row without it reads as: a device session has no E027 lock columns (its scope is the
 * device pair itself), and an older profile.db may lack a column this build added since.
 */
struct SessionColumn {
  const char* name;
  const char* fallback;
  bool on_device_table;
};

constexpr SessionColumn kSessionColumns[] = {
    {"session_epoch", "1", true},
    {"master_psk_b64", "NULL", true},
    {"psk_fingerprint", "NULL", true},
    {"psk_verified_at", "NULL", true},
    {"retired_psks_json", "NULL", true},
    {"key_scope", "'account'", false},
    {"thread_kem_pk_b64", "NULL", false},
    {"thread_kem_sk_b64", "NULL", false},
    {"peer_thread_kem_pk_b64", "NULL", false},
    {"last_psk_rotate_at", "NULL", false},
    {"psk_rotate_msg_count", "0", false},
    {"last_rotation_id", "NULL", false},
    {"auto_key_session_id", "NULL", true},
    {"auto_key_init_pending", "NULL", true},
    {"auto_key_peer_heard_ms", "NULL", true},
    {"restart_pending", "0", true},
};

using HasColumn = std::function<bool(const char* column)>;

bool EveryColumn(const char* /*column*/) {
  return true;
}

/**
 * The SELECT of `table`'s sessions, one row shape for both tables (SessionFromRow): the key, kSessionColumns, the
 * peer device. A column `has` does not name reads as its fallback.
 */
std::string SessionSelect(const char* table, const HasColumn& has) {
  const bool device = std::string_view(table) == kDeviceSessionsTable;
  std::string sql = "SELECT peer_identity_kind, peer_identity_value, channel";
  for (const SessionColumn& column : kSessionColumns) {
    sql += ", ";
    sql += (device && !column.on_device_table) || !has(column.name) ? column.fallback : column.name;
  }
  sql += device ? ", peer_device" : ", ''";
  return sql + " FROM " + table;
}

std::optional<std::string> OptText(sqlite3_stmt* stmt, const int column) {
  if (const unsigned char* text = sqlite3_column_text(stmt, column)) {
    return std::string(reinterpret_cast<const char*>(text));
  }
  return std::nullopt;
}

std::optional<int64_t> OptInt64(sqlite3_stmt* stmt, const int column) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    return std::nullopt;
  }
  return sqlite3_column_int64(stmt, column);
}

/** A row of SessionSelect, as stored (sealed). */
PeerSessionRecord SessionFromRow(sqlite3_stmt* stmt) {
  PeerSessionRecord record;
  record.key.peer_identity_kind = OptText(stmt, 0).value_or(std::string{});
  record.key.peer_identity_value = OptText(stmt, 1).value_or(std::string{});
  record.key.channel = ChannelFromDb(OptText(stmt, 2).value_or(std::string{}));
  record.session_epoch = static_cast<uint32_t>(sqlite3_column_int(stmt, 3));
  record.master_psk_b64 = OptText(stmt, 4);
  record.psk_fingerprint = OptText(stmt, 5);
  record.psk_verified_at = OptInt64(stmt, 6);
  record.retired_psks = ParseRetiredPsks(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7)));
  if (auto scope = OptText(stmt, 8)) {
    record.key_scope = PublicKeyScopeFromString(*scope);
  }
  record.thread_kem_pk_b64 = OptText(stmt, 9);
  record.thread_kem_sk_b64 = OptText(stmt, 10);
  record.peer_thread_kem_pk_b64 = OptText(stmt, 11);
  record.last_psk_rotate_at = OptInt64(stmt, 12);
  record.psk_rotate_msg_count = static_cast<uint32_t>(sqlite3_column_int(stmt, 13));
  record.last_rotation_id = OptText(stmt, 14);
  record.auto_key_session_id = OptText(stmt, 15);
  record.auto_key_init_pending = OptText(stmt, 16);
  record.auto_key_peer_heard_ms = OptInt64(stmt, 17);
  record.restart_pending = sqlite3_column_int(stmt, 18) != 0;
  record.key.peer_device = OptText(stmt, 19).value_or(std::string{});
  return record;
}

bool HasSecrets(const PeerSessionRecord& record) {
  return record.master_psk_b64 || !record.retired_psks.empty() || record.thread_kem_sk_b64;
}

/** `table`'s sessions as stored, read from whatever shape `db` gives it; none when it is not there or not keyed. */
Roe<void> ReadStoredTable(sqlite3* db, const char* table, std::vector<PeerSessionRecord>& rows) {
  auto exists = SqliteTableExists(db, table);
  if (!exists) {
    return exists.error();
  }
  if (!*exists) {
    return {};
  }
  const HasColumn has = [db, table](const char* column) {
    auto present = SqliteColumnExists(db, table, column);
    return present && *present;
  };
  const bool device = std::string_view(table) == kDeviceSessionsTable;
  for (const char* key : {"peer_identity_kind", "peer_identity_value", "channel", device ? "peer_device" : "channel"}) {
    if (!has(key)) {
      return {}; // not keyed as a session is today: it names none
    }
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, (SessionSelect(table, has) + ";").c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    const std::string message = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    return Error(std::string(table) + ": " + message);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    rows.push_back(SessionFromRow(stmt));
  }
  sqlite3_finalize(stmt);
  return {};
}

void BindOptText(sqlite3_stmt* stmt, const int index, const std::optional<std::string>& value) {
  if (value) {
    sqlite3_bind_text(stmt, index, value->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, index);
  }
}

void BindOptInt64(sqlite3_stmt* stmt, const int index, const std::optional<int64_t>& value) {
  if (value) {
    sqlite3_bind_int64(stmt, index, *value);
  } else {
    sqlite3_bind_null(stmt, index);
  }
}

std::string RetiredPsksJson(const std::vector<RetiredPskEntry>& retired_psks) {
  std::vector<Value> retired_elements;
  retired_elements.reserve(retired_psks.size());
  for (const RetiredPskEntry& entry : retired_psks) {
    Object item;
    item.setJsonUInt("epoch", entry.epoch);
    item.set("master_psk_b64", entry.master_psk_b64);
    item.set("retired_at", entry.retired_at);
    retired_elements.push_back(ObjectValue(std::move(item)));
  }
  return retired_elements.empty() ? "[]" : DumpJson(ArrayValue(std::move(retired_elements)));
}

Roe<void> StepToDone(sqlite3_stmt* stmt, const char* failure) {
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    return Error(failure);
  }
  return {};
}

Roe<void> SaveAccountSession(sqlite3* db, const PeerSessionRecord& to_save, const std::string& retired_json) {
  const char* sql =
      "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, local_thread_id, session_epoch, "
      "master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json, key_scope, thread_kem_pk_b64, "
      "thread_kem_sk_b64, peer_thread_kem_pk_b64, last_psk_rotate_at, psk_rotate_msg_count, last_rotation_id, "
      "auto_key_session_id, auto_key_init_pending, auto_key_peer_heard_ms, restart_pending) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(peer_identity_kind, peer_identity_value, channel) DO UPDATE SET "
      "session_epoch=excluded.session_epoch, master_psk_b64=excluded.master_psk_b64, "
      "psk_fingerprint=excluded.psk_fingerprint, psk_verified_at=excluded.psk_verified_at, "
      "retired_psks_json=excluded.retired_psks_json, key_scope=excluded.key_scope, "
      "thread_kem_pk_b64=excluded.thread_kem_pk_b64, thread_kem_sk_b64=excluded.thread_kem_sk_b64, "
      "peer_thread_kem_pk_b64=excluded.peer_thread_kem_pk_b64, last_psk_rotate_at=excluded.last_psk_rotate_at, "
      "psk_rotate_msg_count=excluded.psk_rotate_msg_count, last_rotation_id=excluded.last_rotation_id, "
      "auto_key_session_id=excluded.auto_key_session_id, auto_key_init_pending=excluded.auto_key_init_pending, "
      "auto_key_peer_heard_ms=excluded.auto_key_peer_heard_ms, restart_pending=excluded.restart_pending;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return Error("Failed to prepare PSK save");
  }
  sqlite3_bind_text(stmt, 1, to_save.key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, to_save.key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(to_save.key.channel).c_str(), -1, SQLITE_TRANSIENT);
  // No thread yet (a session for the control lane, or opened before the chat): a placeholder unique per
  // target — local_thread_id is unique, and a shared "pending" made the second such peer fail to save.
  const std::string pending_thread = "pending:" + to_save.key.peer_identity_kind + ":" +
                                     to_save.key.peer_identity_value + ":" + ChannelToDb(to_save.key.channel);
  sqlite3_bind_text(stmt, 4, pending_thread.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, static_cast<int>(to_save.session_epoch));
  BindOptText(stmt, 6, to_save.master_psk_b64);
  BindOptText(stmt, 7, to_save.psk_fingerprint);
  BindOptInt64(stmt, 8, to_save.psk_verified_at);
  sqlite3_bind_text(stmt, 9, retired_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 10, PublicKeyScopeToString(to_save.key_scope), -1, SQLITE_TRANSIENT);
  BindOptText(stmt, 11, to_save.thread_kem_pk_b64);
  BindOptText(stmt, 12, to_save.thread_kem_sk_b64);
  BindOptText(stmt, 13, to_save.peer_thread_kem_pk_b64);
  BindOptInt64(stmt, 14, to_save.last_psk_rotate_at);
  sqlite3_bind_int(stmt, 15, static_cast<int>(to_save.psk_rotate_msg_count));
  BindOptText(stmt, 16, to_save.last_rotation_id);
  BindOptText(stmt, 17, to_save.auto_key_session_id);
  BindOptText(stmt, 18, to_save.auto_key_init_pending);
  BindOptInt64(stmt, 19, to_save.auto_key_peer_heard_ms);
  sqlite3_bind_int(stmt, 20, to_save.restart_pending ? 1 : 0);
  return StepToDone(stmt, "Failed to save PSK row");
}

Roe<void> SaveDeviceSession(sqlite3* db, const PeerSessionRecord& to_save, const std::string& retired_json) {
  // A session moved to another epoch numbers from 1 there (its old epoch's count is the old epoch's).
  const char* sql =
      "INSERT INTO device_sessions (peer_identity_kind, peer_identity_value, channel, peer_device, session_epoch, "
      "master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json, auto_key_session_id, "
      "auto_key_init_pending, auto_key_peer_heard_ms, restart_pending) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(peer_identity_kind, peer_identity_value, channel, peer_device) DO UPDATE SET "
      "next_outgoing_seq = CASE WHEN excluded.session_epoch != device_sessions.session_epoch THEN 1 "
      "ELSE device_sessions.next_outgoing_seq END, "
      "session_epoch=excluded.session_epoch, master_psk_b64=excluded.master_psk_b64, "
      "psk_fingerprint=excluded.psk_fingerprint, psk_verified_at=excluded.psk_verified_at, "
      "retired_psks_json=excluded.retired_psks_json, auto_key_session_id=excluded.auto_key_session_id, "
      "auto_key_init_pending=excluded.auto_key_init_pending, auto_key_peer_heard_ms=excluded.auto_key_peer_heard_ms, "
      "restart_pending=excluded.restart_pending;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return Error("Failed to prepare device session save");
  }
  sqlite3_bind_text(stmt, 1, to_save.key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, to_save.key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(to_save.key.channel).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, to_save.key.peer_device.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 5, static_cast<int>(to_save.session_epoch));
  BindOptText(stmt, 6, to_save.master_psk_b64);
  BindOptText(stmt, 7, to_save.psk_fingerprint);
  BindOptInt64(stmt, 8, to_save.psk_verified_at);
  sqlite3_bind_text(stmt, 9, retired_json.c_str(), -1, SQLITE_TRANSIENT);
  BindOptText(stmt, 10, to_save.auto_key_session_id);
  BindOptText(stmt, 11, to_save.auto_key_init_pending);
  BindOptInt64(stmt, 12, to_save.auto_key_peer_heard_ms);
  sqlite3_bind_int(stmt, 13, to_save.restart_pending ? 1 : 0);
  return StepToDone(stmt, "Failed to save device session");
}

} // namespace

SqlitePeerSessionStore::SqlitePeerSessionStore(std::string profile_db_path, std::string profile_id)
    : profile_db_path_(std::move(profile_db_path)), profile_id_(std::move(profile_id)) {
  redirectLogger("SqlitePeerSessionStore");
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }
}

SqlitePeerSessionStore::~SqlitePeerSessionStore() {
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

Roe<void> SqlitePeerSessionStore::SetDek(ByteVector dek) {
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

void SqlitePeerSessionStore::ClearDek() {
  std::lock_guard lock(mutex_);
  if (!dek_.empty()) {
    sodium_memzero(dek_.data(), dek_.size());
    dek_.clear();
  }
}

Roe<void> SqlitePeerSessionStore::RequireDek() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return AppError::Pin(Err::Pin::Required, "PSK store DEK not set (unlock profile vault first)");
  }
  return {};
}

Roe<std::string> SqlitePeerSessionStore::EncryptFieldB64(const std::string& plaintext_b64, const char* aad_kind) const {
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

Roe<std::string> SqlitePeerSessionStore::DecryptFieldB64(const std::string& ciphertext_b64,
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

Roe<std::string> SqlitePeerSessionStore::EncryptPskB64(const std::string& plaintext_b64) const {
  return EncryptFieldB64(plaintext_b64, "psk");
}

Roe<std::string> SqlitePeerSessionStore::DecryptPskB64(const std::string& ciphertext_b64) const {
  return DecryptFieldB64(ciphertext_b64, "psk");
}

Roe<PeerSessionRecord> SqlitePeerSessionStore::DecryptRecord(PeerSessionRecord record) const {
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

Roe<PeerSessionRecord> SqlitePeerSessionStore::EncryptRecord(PeerSessionRecord record) const {
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

Roe<sqlite3*> SqlitePeerSessionStore::OpenDb() const {
  std::lock_guard lock(mutex_);
  if (db_) {
    return db_;
  }
  if (auto gate = chat_store_gate::RequireOpen(profile_db_path_); !gate) {
    return gate.error();
  }
  if (sqlite3_open(profile_db_path_.c_str(), &db_) != SQLITE_OK) {
    return Error("Failed to open profile.db for PSK store");
  }
  // profile.db has other writers (group rosters, call sessions): wait for them, as they wait for us.
  sqlite3_busy_timeout(db_, 5000);
  if (auto schema = EnsureSchema(db_); !schema) {
    return schema.error();
  }
  return db_;
}

Roe<void> SqlitePeerSessionStore::EnsureSchema(sqlite3* db) {
  char* err = nullptr;
  if (sqlite3_exec(db, kDeviceSessionsSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "device_sessions schema failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<std::vector<PeerSessionRecord>> SqlitePeerSessionStore::ReadStored(sqlite3* db) {
  std::vector<PeerSessionRecord> rows;
  for (const char* table : {kAccountSessionsTable, kDeviceSessionsTable}) {
    if (auto read = ReadStoredTable(db, table, rows); !read) {
      return read.error();
    }
  }
  return rows;
}

Roe<void> SqlitePeerSessionStore::WriteStored(sqlite3* db, const PeerSessionRecord& sealed) {
  const std::string retired_json = RetiredPsksJson(sealed.retired_psks);
  return sealed.key.peer_device.empty() ? SaveAccountSession(db, sealed, retired_json)
                                        : SaveDeviceSession(db, sealed, retired_json);
}

Roe<std::optional<PeerSessionRecord>> SqlitePeerSessionStore::Load(const PeerSessionKey& key) const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  const bool device = !key.peer_device.empty();
  const std::string sql = SessionSelect(device ? kDeviceSessionsTable : kAccountSessionsTable, EveryColumn) +
                          " WHERE peer_identity_kind = ? AND peer_identity_value = ? AND channel = ?" +
                          (device ? " AND peer_device = ?" : "") + " LIMIT 1;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return Error("Failed to prepare PSK load");
  }
  sqlite3_bind_text(stmt, 1, key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(key.channel).c_str(), -1, SQLITE_TRANSIENT);
  if (device) {
    sqlite3_bind_text(stmt, 4, key.peer_device.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::optional<PeerSessionRecord>{};
  }
  PeerSessionRecord record = SessionFromRow(stmt);
  sqlite3_finalize(stmt);
  record.key = key;
  if (!HasSecrets(record)) {
    return std::optional<PeerSessionRecord>(record);
  }
  auto decrypted = DecryptRecord(std::move(record));
  if (!decrypted) {
    return decrypted.error();
  }
  return std::optional<PeerSessionRecord>(*decrypted);
}

Roe<std::vector<PeerSessionRecord>> SqlitePeerSessionStore::List() const {
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  const std::string sql = SessionSelect(kAccountSessionsTable, EveryColumn) + " UNION ALL " +
                          SessionSelect(kDeviceSessionsTable, EveryColumn) + ";";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return Error("Failed to prepare PSK list");
  }
  std::vector<PeerSessionRecord> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PeerSessionRecord record = SessionFromRow(stmt);
    if (!HasSecrets(record)) {
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

Roe<void> SqlitePeerSessionStore::Save(const PeerSessionRecord& record) {
  PeerSessionRecord to_save = record;
  ApplyFingerprint(to_save);
  PskBundleCodec::CapRetiredTail(to_save.retired_psks, to_save.session_epoch);

  auto encrypted = EncryptRecord(std::move(to_save));
  if (!encrypted) {
    return encrypted.error();
  }
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  return WriteStored(*db, *encrypted);
}

Roe<uint64_t> SqlitePeerSessionStore::AllocateSenderSeq(const PeerSessionKey& key) {
  if (key.peer_device.empty()) {
    return Error("AllocateSenderSeq: a device-pair session only (the account's numbering is the thread's)");
  }
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "UPDATE device_sessions SET next_outgoing_seq = next_outgoing_seq + 1 "
                         "WHERE peer_identity_kind = ? AND peer_identity_value = ? AND channel = ? AND peer_device = ? "
                         "RETURNING next_outgoing_seq - 1;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare device seq allocation");
  }
  sqlite3_bind_text(stmt, 1, key.peer_identity_kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, key.peer_identity_value.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, ChannelToDb(key.channel).c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, key.peer_device.c_str(), -1, SQLITE_TRANSIENT);
  const int step = sqlite3_step(stmt);
  if (step != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return Error("No device session to number in");
  }
  const auto seq = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  sqlite3_finalize(stmt);
  return seq;
}

} // namespace pbr
