#include "feature/messaging/SqlitePskSessionStore.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/PskBundleCodec.h"
#include "base/crypto/PskFingerprint.h"

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sqlite3.h>

namespace pbr {

namespace {

std::string ChannelToDb(const CryptoChannel channel) {
  return CryptoChannelToString(channel);
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

SqlitePskSessionStore::SqlitePskSessionStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {
  redirectLogger("SqlitePskSessionStore");
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
      "SELECT session_epoch, master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json FROM chat_targets "
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
  if (sqlite3_column_text(stmt, 4)) {
    const nlohmann::json retired =
        nlohmann::json::parse(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4)), nullptr, false);
    if (retired.is_array()) {
      for (const auto& item : retired) {
        RetiredPskEntry entry;
        entry.epoch = item.value("epoch", 0u);
        entry.master_psk_b64 = item.value("master_psk_b64", std::string{});
        entry.retired_at = item.value("retired_at", static_cast<int64_t>(0));
        record.retired_psks.push_back(std::move(entry));
      }
    }
  }
  sqlite3_finalize(stmt);
  return std::optional<PskSessionRecord>(record);
}

Roe<void> SqlitePskSessionStore::Save(const PskSessionRecord& record) {
  PskSessionRecord to_save = record;
  ApplyFingerprint(to_save);
  PskBundleCodec::CapRetiredTail(to_save.retired_psks, to_save.session_epoch);

  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  nlohmann::json retired = nlohmann::json::array();
  for (const RetiredPskEntry& entry : to_save.retired_psks) {
    retired.push_back(
        {{"epoch", entry.epoch}, {"master_psk_b64", entry.master_psk_b64}, {"retired_at", entry.retired_at}});
  }
  const std::string retired_json = retired.empty() ? "[]" : retired.dump();
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO chat_targets (peer_identity_kind, peer_identity_value, channel, local_thread_id, session_epoch, "
      "master_psk_b64, psk_fingerprint, psk_verified_at, retired_psks_json) "
      "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(peer_identity_kind, peer_identity_value, channel) DO UPDATE SET "
      "session_epoch=excluded.session_epoch, master_psk_b64=excluded.master_psk_b64, "
      "psk_fingerprint=excluded.psk_fingerprint, psk_verified_at=excluded.psk_verified_at, "
      "retired_psks_json=excluded.retired_psks_json;";
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
  } else {
    record.retired_psks = bundle.retired_epochs;
  }
  PskBundleCodec::CapRetiredTail(record.retired_psks, record.session_epoch);
  return Save(record);
}

} // namespace pbr
