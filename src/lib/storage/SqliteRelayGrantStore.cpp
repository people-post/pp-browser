#include "lib/storage/SqliteRelayGrantStore.h"

#include "lib/storage/SqliteChatStoreGate.h"

#include <sqlite3.h>

#include <memory>
#include <utility>

#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr const char* kSchemaSql = R"SQL(
CREATE TABLE IF NOT EXISTS relay_grants (
  direction TEXT NOT NULL,
  peer_account TEXT NOT NULL,
  recipient_account TEXT NOT NULL,
  sender_account TEXT NOT NULL,
  issued_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  signature TEXT NOT NULL,
  confirmed INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (direction, peer_account)
);
)SQL";

constexpr const char kReceived[] = "received";
constexpr const char kIssued[] = "issued";

struct DbCloser {
  void operator()(sqlite3* db) const { sqlite3_close(db); }
};

struct StmtCloser {
  void operator()(sqlite3_stmt* stmt) const { sqlite3_finalize(stmt); }
};

std::string ColumnText(sqlite3_stmt* stmt, const int column) {
  const unsigned char* text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char*>(text) : std::string();
}

} // namespace

SqliteRelayGrantStore::SqliteRelayGrantStore(std::string profile_db_path)
    : profile_db_path_(std::move(profile_db_path)) {}

Roe<sqlite3*> SqliteRelayGrantStore::OpenDb() const {
  if (auto gate = chat_store_gate::RequireOpen(profile_db_path_); !gate) {
    return gate.error();
  }
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for relay grants");
  }
  sqlite3_busy_timeout(db, 5000);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(db, kSchemaSql, nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "relay_grants schema failed";
      sqlite3_free(err);
      sqlite3_close(db);
      return Error(message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<std::optional<IRelayGrantStore::Issued>> SqliteRelayGrantStore::Get(const char* direction,
                                                                        const std::string& peer) {
  std::lock_guard lock(mutex_);
  auto opened = OpenDb();
  if (!opened) {
    return opened.error();
  }
  std::unique_ptr<sqlite3, DbCloser> db(*opened);
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db.get(),
                         "SELECT recipient_account, sender_account, issued_at, expires_at, signature, confirmed "
                         "FROM relay_grants WHERE direction = ?1 AND peer_account = ?2",
                         -1, &raw, nullptr) != SQLITE_OK) {
    sqlite3_finalize(raw);
    return Error("relay_grants select prepare failed");
  }
  std::unique_ptr<sqlite3_stmt, StmtCloser> stmt(raw);
  sqlite3_bind_text(raw, 1, direction, -1, SQLITE_STATIC);
  sqlite3_bind_text(raw, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(raw);
  if (rc == SQLITE_DONE) {
    return std::optional<Issued>{};
  }
  if (rc != SQLITE_ROW) {
    return Error("relay_grants select failed");
  }
  Issued row;
  row.grant.recipient_account_id = ColumnText(raw, 0);
  row.grant.sender_account_id = ColumnText(raw, 1);
  row.grant.issued_at = sqlite3_column_int64(raw, 2);
  row.grant.expires_at = sqlite3_column_int64(raw, 3);
  row.grant.signature = ColumnText(raw, 4);
  row.confirmed = sqlite3_column_int(raw, 5) != 0;
  return std::optional<Issued>(std::move(row));
}

Roe<void> SqliteRelayGrantStore::Put(const char* direction, const std::string& peer, const RelayGrant& grant,
                                     const bool confirmed, const bool only_if_newer) {
  if (peer.empty()) {
    return Error("relay grant without a peer");
  }
  std::lock_guard lock(mutex_);
  auto opened = OpenDb();
  if (!opened) {
    return opened.error();
  }
  std::unique_ptr<sqlite3, DbCloser> db(*opened);
  const char* sql =
      only_if_newer
          ? "INSERT INTO relay_grants (direction, peer_account, recipient_account, sender_account, issued_at, "
            "expires_at, signature, confirmed) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8) "
            "ON CONFLICT (direction, peer_account) DO UPDATE SET recipient_account = excluded.recipient_account, "
            "sender_account = excluded.sender_account, issued_at = excluded.issued_at, "
            "expires_at = excluded.expires_at, signature = excluded.signature, confirmed = excluded.confirmed "
            "WHERE excluded.issued_at > relay_grants.issued_at"
          : "INSERT OR REPLACE INTO relay_grants (direction, peer_account, recipient_account, sender_account, "
            "issued_at, expires_at, signature, confirmed) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)";
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db.get(), sql, -1, &raw, nullptr) != SQLITE_OK) {
    sqlite3_finalize(raw);
    return Error("relay_grants upsert prepare failed");
  }
  std::unique_ptr<sqlite3_stmt, StmtCloser> stmt(raw);
  sqlite3_bind_text(raw, 1, direction, -1, SQLITE_STATIC);
  sqlite3_bind_text(raw, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(raw, 3, grant.recipient_account_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(raw, 4, grant.sender_account_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(raw, 5, grant.issued_at);
  sqlite3_bind_int64(raw, 6, grant.expires_at);
  sqlite3_bind_text(raw, 7, grant.signature.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(raw, 8, confirmed ? 1 : 0);
  if (sqlite3_step(raw) != SQLITE_DONE) {
    return Error("relay_grants upsert failed");
  }
  return {};
}

Roe<void> SqliteRelayGrantStore::Drop(const char* direction, const std::string& peer) {
  std::lock_guard lock(mutex_);
  auto opened = OpenDb();
  if (!opened) {
    return opened.error();
  }
  std::unique_ptr<sqlite3, DbCloser> db(*opened);
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(db.get(), "DELETE FROM relay_grants WHERE direction = ?1 AND peer_account = ?2", -1, &raw,
                         nullptr) != SQLITE_OK) {
    sqlite3_finalize(raw);
    return Error("relay_grants delete prepare failed");
  }
  std::unique_ptr<sqlite3_stmt, StmtCloser> stmt(raw);
  sqlite3_bind_text(raw, 1, direction, -1, SQLITE_STATIC);
  sqlite3_bind_text(raw, 2, peer.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(raw) != SQLITE_DONE) {
    return Error("relay_grants delete failed");
  }
  return {};
}

Roe<std::optional<RelayGrant>> SqliteRelayGrantStore::Received(const std::string& recipient_account_id) {
  auto row = Get(kReceived, recipient_account_id);
  if (!row) {
    return row.error();
  }
  if (!*row) {
    return std::optional<RelayGrant>{};
  }
  return std::optional<RelayGrant>((*row)->grant);
}

Roe<void> SqliteRelayGrantStore::PutReceived(const RelayGrant& grant) {
  return Put(kReceived, grant.recipient_account_id, grant, false, /*only_if_newer=*/true);
}

Roe<void> SqliteRelayGrantStore::DropReceived(const std::string& recipient_account_id) {
  return Drop(kReceived, recipient_account_id);
}

Roe<std::optional<IRelayGrantStore::Issued>> SqliteRelayGrantStore::IssuedTo(const std::string& sender_account_id) {
  return Get(kIssued, sender_account_id);
}

Roe<void> SqliteRelayGrantStore::PutIssued(const Issued& issued) {
  return Put(kIssued, issued.grant.sender_account_id, issued.grant, issued.confirmed, /*only_if_newer=*/false);
}

Roe<void> SqliteRelayGrantStore::DropIssued(const std::string& sender_account_id) {
  return Drop(kIssued, sender_account_id);
}

} // namespace pbr
