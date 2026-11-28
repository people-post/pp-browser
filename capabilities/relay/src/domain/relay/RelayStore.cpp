#include "domain/relay/RelayStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <utility>

namespace pbr::relay {
namespace {

// Schema 1 (RelayStore::kSchemaVersion). `mailboxes.last_seq` keeps each mailbox's order going across deletes, so a
// device's cursor never points past a reused seq.
constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS mailboxes (
  account_id TEXT PRIMARY KEY,
  last_seq INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS records (
  recipient TEXT NOT NULL,
  seq INTEGER NOT NULL,
  sender TEXT NOT NULL,
  stream_id TEXT NOT NULL,
  index_key INTEGER NOT NULL,
  body BLOB NOT NULL,
  urgent INTEGER NOT NULL,
  created_at_ms INTEGER NOT NULL,
  expires_at_ms INTEGER NOT NULL,
  PRIMARY KEY (recipient, seq)
) WITHOUT ROWID;
CREATE UNIQUE INDEX IF NOT EXISTS records_stream ON records (recipient, sender, stream_id, index_key);
CREATE INDEX IF NOT EXISTS records_expiry ON records (expires_at_ms);
CREATE INDEX IF NOT EXISTS records_created ON records (recipient, created_at_ms);
)sql";

constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

/** A prepared statement, finalized when it goes. */
class Stmt {
public:
  Stmt(sqlite3* db, const char* sql) : db_(db) { rc_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool ok() const { return rc_ == SQLITE_OK && stmt_; }
  Stmt& Text(int i, const std::string& v) {
    sqlite3_bind_text(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& Int(int i, int64_t v) {
    sqlite3_bind_int64(stmt_, i, v);
    return *this;
  }
  Stmt& Blob(int i, const std::vector<uint8_t>& v) {
    sqlite3_bind_blob(stmt_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    return *this;
  }
  /** SQLITE_ROW, SQLITE_DONE or an error. */
  int Step() { return sqlite3_step(stmt_); }
  int64_t ColInt(int i) const { return sqlite3_column_int64(stmt_, i); }
  std::string ColText(int i) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, i));
    return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(stmt_, i))) : std::string();
  }
  std::vector<uint8_t> ColBlob(int i) const {
    const auto* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, i));
    return p ? std::vector<uint8_t>(p, p + sqlite3_column_bytes(stmt_, i)) : std::vector<uint8_t>();
  }
  std::string Message() const { return sqlite3_errmsg(db_); }

private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
  int rc_ = SQLITE_ERROR;
};

Error DbError(sqlite3* db, const std::string& what) {
  return Error("relay store: " + what + ": " + sqlite3_errmsg(db));
}

Roe<void> Exec(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    std::string text = message ? message : "error";
    sqlite3_free(message);
    return Error(std::string("relay store: ") + text);
  }
  return {};
}

/** Rolls back unless committed. */
class Transaction {
public:
  explicit Transaction(sqlite3* db) : db_(db) { began_ = static_cast<bool>(Exec(db, "BEGIN IMMEDIATE")); }
  ~Transaction() {
    if (began_ && !done_) {
      (void)Exec(db_, "ROLLBACK");
    }
  }
  bool began() const { return began_; }
  Roe<void> Commit() {
    done_ = true;
    return Exec(db_, "COMMIT");
  }

private:
  sqlite3* db_;
  bool began_ = false;
  bool done_ = false;
};

constexpr const char* kColumns = "seq, sender, stream_id, index_key, body, urgent, created_at_ms, expires_at_ms";

MailRecord ReadRecord(const Stmt& s) {
  MailRecord r;
  r.seq = static_cast<uint64_t>(s.ColInt(0));
  r.sender_account_id = s.ColText(1);
  r.stream_id = s.ColText(2);
  r.index_key = static_cast<uint64_t>(s.ColInt(3));
  r.body = s.ColBlob(4);
  r.urgent = s.ColInt(5) != 0;
  r.created_at_ms = s.ColInt(6);
  r.expires_at_ms = s.ColInt(7);
  return r;
}

void Restrict(const std::filesystem::path& path, const std::filesystem::perms perms) {
  std::error_code ec;
  std::filesystem::permissions(path, perms, std::filesystem::perm_options::replace, ec);
}

} // namespace

RelayStore::RelayStore(sqlite3* db, std::string path, RelayLimits limits, Clock clock)
    : db_(db), path_(std::move(path)), limits_(limits), clock_(std::move(clock)) {}

RelayStore::~RelayStore() {
  sqlite3_close(db_);
}

int64_t RelayStore::Now() const {
  if (clock_) {
    return clock_();
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Roe<std::unique_ptr<RelayStore>> RelayStore::Open(const std::string& dir, RelayLimits limits, Clock clock) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return Error("relay store: cannot create " + dir + ": " + ec.message());
  }
  Restrict(dir, fs::perms::owner_all);
  const std::string path = (fs::path(dir) / kFileName).string();

  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
    Error error = db ? DbError(db, "cannot open " + path) : Error("relay store: cannot open " + path);
    sqlite3_close(db);
    return error;
  }
  Restrict(path, fs::perms::owner_read | fs::perms::owner_write);
  std::unique_ptr<RelayStore> store(new RelayStore(db, path, limits, std::move(clock)));
  sqlite3_busy_timeout(db, 5000);

  int version = 0;
  {
    Stmt s(db, "PRAGMA user_version");
    if (!s.ok() || s.Step() != SQLITE_ROW) {
      return DbError(db, "cannot read the schema version of " + path);
    }
    version = static_cast<int>(s.ColInt(0));
  }
  if (version > kSchemaVersion) {
    return Error("relay store: " + path + " is from a newer pp-node (schema " + std::to_string(version) +
                 ", this one " + std::to_string(kSchemaVersion) +
                 "); it is left as it is: run that build, or move the file aside");
  }
  if (auto r = Exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"); !r) {
    return r.error();
  }
  if (version < kSchemaVersion) {
    Transaction tx(db);
    if (!tx.began()) {
      return DbError(db, "cannot begin");
    }
    if (auto r = Exec(db, kSchema); !r) {
      return r.error();
    }
    if (auto r = Exec(db, ("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str()); !r) {
      return r.error();
    }
    if (auto r = tx.Commit(); !r) {
      return r.error();
    }
  }
  if (auto expired = store->Expire(); !expired) {
    return expired.error();
  }
  return store;
}

Roe<RelayStore::Usage> RelayStore::UsageLocked(const std::string& recipient, const int64_t now) {
  Stmt s(db_, "SELECT COUNT(*), COALESCE(SUM(LENGTH(body)), 0) FROM records WHERE recipient = ? AND expires_at_ms > ?");
  s.Text(1, recipient).Int(2, now);
  if (!s.ok() || s.Step() != SQLITE_ROW) {
    return DbError(db_, "usage");
  }
  return Usage{s.ColInt(0), s.ColInt(1)};
}

Roe<RelayStore::Usage> RelayStore::UsageOf(const std::string& recipient) {
  std::lock_guard lock(mu_);
  return UsageLocked(recipient, Now());
}

Roe<RelayStore::PutResult> RelayStore::Put(const std::string& recipient, const MailRecord& record,
                                           const int64_t ttl_ms) {
  if (record.index_key > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Error("relay store: index key out of range");
  }
  std::lock_guard lock(mu_);
  const int64_t now = Now();
  int64_t retention = limits_.retention_days * kDayMs;
  if (record.urgent) {
    retention = std::min(retention, kUrgentRetentionMs);
  }
  if (ttl_ms > 0) {
    retention = std::min(retention, std::max(ttl_ms, kMinTtlMs));
  }

  Transaction tx(db_);
  if (!tx.began()) {
    return DbError(db_, "cannot begin");
  }
  const auto index_key = static_cast<int64_t>(record.index_key);
  {
    // An expired record of the same key never blocks a new one.
    Stmt s(db_,
           "DELETE FROM records WHERE recipient = ? AND sender = ? AND stream_id = ? AND index_key = ? AND "
           "expires_at_ms <= ?");
    s.Text(1, recipient).Text(2, record.sender_account_id).Text(3, record.stream_id).Int(4, index_key).Int(5, now);
    if (!s.ok() || s.Step() != SQLITE_DONE) {
      return DbError(db_, "put");
    }
  }
  {
    Stmt s(db_, "SELECT 1 FROM records WHERE recipient = ? AND sender = ? AND stream_id = ? AND index_key = ?");
    s.Text(1, recipient).Text(2, record.sender_account_id).Text(3, record.stream_id).Int(4, index_key);
    if (!s.ok()) {
      return DbError(db_, "put");
    }
    if (s.Step() == SQLITE_ROW) {
      return PutResult{PutOutcome::Duplicate, 0};
    }
  }
  auto usage = UsageLocked(recipient, now);
  if (!usage) {
    return usage.error();
  }
  if (usage->records >= limits_.max_records) {
    return PutResult{PutOutcome::FullRecords, 0};
  }
  if (usage->bytes + static_cast<int64_t>(record.body.size()) > limits_.max_bytes) {
    return PutResult{PutOutcome::FullBytes, 0};
  }
  int64_t seq = 0;
  {
    Stmt insert(db_, "INSERT OR IGNORE INTO mailboxes (account_id, last_seq) VALUES (?, 0)");
    insert.Text(1, recipient);
    Stmt bump(db_, "UPDATE mailboxes SET last_seq = last_seq + 1 WHERE account_id = ?");
    bump.Text(1, recipient);
    Stmt read(db_, "SELECT last_seq FROM mailboxes WHERE account_id = ?");
    read.Text(1, recipient);
    if (!insert.ok() || insert.Step() != SQLITE_DONE || !bump.ok() || bump.Step() != SQLITE_DONE || !read.ok() ||
        read.Step() != SQLITE_ROW) {
      return DbError(db_, "put");
    }
    seq = read.ColInt(0);
  }
  {
    Stmt s(db_,
           "INSERT INTO records (recipient, seq, sender, stream_id, index_key, body, urgent, created_at_ms, "
           "expires_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.Text(1, recipient)
        .Int(2, seq)
        .Text(3, record.sender_account_id)
        .Text(4, record.stream_id)
        .Int(5, index_key)
        .Blob(6, record.body)
        .Int(7, record.urgent ? 1 : 0)
        .Int(8, now)
        .Int(9, now + retention);
    if (!s.ok() || s.Step() != SQLITE_DONE) {
      return DbError(db_, "put");
    }
  }
  if (auto r = tx.Commit(); !r) {
    return r.error();
  }
  return PutResult{PutOutcome::Stored, static_cast<uint64_t>(seq)};
}

Roe<RelayStore::Page> RelayStore::Poll(const std::string& recipient, const uint64_t after_seq, const size_t max_records,
                                       const size_t max_bytes) {
  std::lock_guard lock(mu_);
  Page page;
  page.next_after = after_seq;
  if (max_records == 0) {
    return page;
  }
  const std::string sql = std::string("SELECT ") + kColumns +
                          " FROM records WHERE recipient = ? AND seq > ? AND expires_at_ms > ? ORDER BY seq LIMIT ?";
  Stmt s(db_, sql.c_str());
  s.Text(1, recipient)
      .Int(2, static_cast<int64_t>(std::min<uint64_t>(after_seq, std::numeric_limits<int64_t>::max())))
      .Int(3, Now())
      .Int(4, static_cast<int64_t>(max_records) + 1);
  if (!s.ok()) {
    return DbError(db_, "poll");
  }
  size_t bytes = 0;
  int rc = SQLITE_DONE;
  while ((rc = s.Step()) == SQLITE_ROW) {
    if (page.records.size() == max_records || bytes >= max_bytes) {
      page.more = true;
      break;
    }
    page.records.push_back(ReadRecord(s));
    bytes += page.records.back().body.size();
    page.next_after = page.records.back().seq;
  }
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    return DbError(db_, "poll");
  }
  return page;
}

Roe<RelayStore::StreamPage> RelayStore::Stream(const std::string& recipient, const StreamQuery& query) {
  std::lock_guard lock(mu_);
  StreamPage page;
  if (query.limit == 0) {
    return page;
  }
  const auto clamp = [](const uint64_t v) {
    return static_cast<int64_t>(std::min<uint64_t>(v, std::numeric_limits<int64_t>::max()));
  };
  const std::string sql = std::string("SELECT ") + kColumns +
                          " FROM records WHERE recipient = ? AND sender = ? AND stream_id = ? AND index_key >= ? AND "
                          "index_key <= ? AND expires_at_ms > ? ORDER BY index_key " +
                          (query.descending ? "DESC" : "ASC") + " LIMIT ?";
  Stmt s(db_, sql.c_str());
  s.Text(1, recipient)
      .Text(2, query.sender_account_id)
      .Text(3, query.stream_id)
      .Int(4, clamp(query.min_index_key.value_or(0)))
      .Int(5, clamp(query.max_index_key.value_or(std::numeric_limits<uint64_t>::max())))
      .Int(6, Now())
      .Int(7, static_cast<int64_t>(query.limit) + 1);
  if (!s.ok()) {
    return DbError(db_, "stream");
  }
  int rc = SQLITE_DONE;
  while ((rc = s.Step()) == SQLITE_ROW) {
    if (page.records.size() == query.limit) {
      page.more = true;
      break;
    }
    page.records.push_back(ReadRecord(s));
  }
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    return DbError(db_, "stream");
  }
  return page;
}

Roe<int64_t> RelayStore::ClearBefore(const std::string& recipient, const int64_t before_ms) {
  std::lock_guard lock(mu_);
  Stmt s(db_, "DELETE FROM records WHERE recipient = ? AND created_at_ms < ?");
  s.Text(1, recipient).Int(2, before_ms);
  if (!s.ok() || s.Step() != SQLITE_DONE) {
    return DbError(db_, "clear");
  }
  return static_cast<int64_t>(sqlite3_changes(db_));
}

Roe<int64_t> RelayStore::DeleteMailbox(const std::string& recipient) {
  std::lock_guard lock(mu_);
  // The mailbox's seq stays, so a device's cursor stays meaningful if the account comes back.
  Stmt s(db_, "DELETE FROM records WHERE recipient = ?");
  s.Text(1, recipient);
  if (!s.ok() || s.Step() != SQLITE_DONE) {
    return DbError(db_, "delete");
  }
  return static_cast<int64_t>(sqlite3_changes(db_));
}

Roe<int64_t> RelayStore::Expire() {
  std::lock_guard lock(mu_);
  Stmt s(db_, "DELETE FROM records WHERE expires_at_ms <= ?");
  s.Int(1, Now());
  if (!s.ok() || s.Step() != SQLITE_DONE) {
    return DbError(db_, "expire");
  }
  return static_cast<int64_t>(sqlite3_changes(db_));
}

} // namespace pbr::relay
