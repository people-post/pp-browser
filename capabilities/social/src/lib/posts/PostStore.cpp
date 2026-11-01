#include "lib/posts/PostStore.h"

#include "lib/posts/PostEntry.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace pbr {

namespace {

constexpr const char* kSchema =
    "CREATE TABLE IF NOT EXISTS post_entries ("
    " author TEXT NOT NULL, lane BLOB NOT NULL, seq INTEGER NOT NULL, entry_hash BLOB NOT NULL,"
    " kind INTEGER NOT NULL, claimed_at INTEGER NOT NULL, size INTEGER NOT NULL, signed BLOB,"
    " stub INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (author, lane, seq, entry_hash));"
    "CREATE TABLE IF NOT EXISTS post_heads ("
    " author TEXT PRIMARY KEY, issued_at INTEGER NOT NULL, canonical BLOB NOT NULL, signature BLOB NOT NULL);"
    "CREATE TABLE IF NOT EXISTS post_own_lanes ("
    " device TEXT PRIMARY KEY, lane BLOB NOT NULL, next_seq INTEGER NOT NULL, last_hash BLOB NOT NULL,"
    " last_claimed_at INTEGER NOT NULL);";

/** A prepared statement, finalized when it goes. */
class Stmt {
public:
  Stmt(sqlite3* db, const char* sql) { ok_ = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) == SQLITE_OK; }
  ~Stmt() { sqlite3_finalize(stmt_); }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  bool ok() const { return ok_; }
  Stmt& Text(int i, const std::string& value) {
    sqlite3_bind_text(stmt_, i, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return *this;
  }
  Stmt& Blob(int i, const uint8_t* data, size_t size) {
    sqlite3_bind_blob(stmt_, i, data, static_cast<int>(size), SQLITE_TRANSIENT);
    return *this;
  }
  template <size_t N>
  Stmt& Blob(int i, const std::array<uint8_t, N>& value) {
    return Blob(i, value.data(), N);
  }
  Stmt& Blob(int i, const ByteVector& value) { return Blob(i, value.data(), value.size()); }
  Stmt& Null(int i) {
    sqlite3_bind_null(stmt_, i);
    return *this;
  }
  Stmt& Int(int i, int64_t value) {
    sqlite3_bind_int64(stmt_, i, value);
    return *this;
  }
  bool Row() { return sqlite3_step(stmt_) == SQLITE_ROW; }
  bool Done() { return sqlite3_step(stmt_) == SQLITE_DONE; }

  int64_t ColInt(int i) const { return sqlite3_column_int64(stmt_, i); }
  std::string ColText(int i) const {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, i));
    return text ? std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt_, i))) : std::string();
  }
  ByteVector ColBlob(int i) const {
    const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(stmt_, i));
    return data ? ByteVector(data, data + sqlite3_column_bytes(stmt_, i)) : ByteVector();
  }
  bool ColIsNull(int i) const { return sqlite3_column_type(stmt_, i) == SQLITE_NULL; }
  template <size_t N>
  std::array<uint8_t, N> ColArray(int i) const {
    std::array<uint8_t, N> out{};
    const ByteVector bytes = ColBlob(i);
    if (bytes.size() == N) {
      std::copy(bytes.begin(), bytes.end(), out.begin());
    }
    return out;
  }

private:
  sqlite3_stmt* stmt_ = nullptr;
  bool ok_ = false;
};

constexpr const char* kEntryColumns = "author, lane, seq, entry_hash, kind, claimed_at, size, signed, stub";

StoredPostEntry EntryFromRow(const Stmt& row) {
  StoredPostEntry entry;
  entry.author = row.ColText(0);
  entry.lane = row.ColArray<kPostLaneBytes>(1);
  entry.seq = static_cast<uint64_t>(row.ColInt(2));
  entry.entry_hash = row.ColArray<kPostHashBytes>(3);
  entry.kind = static_cast<uint8_t>(row.ColInt(4));
  entry.claimed_at = row.ColInt(5);
  entry.size = static_cast<uint64_t>(row.ColInt(6));
  if (!row.ColIsNull(7)) {
    entry.signed_bytes = row.ColBlob(7);
  }
  entry.stub = static_cast<PostStub>(row.ColInt(8));
  return entry;
}

} // namespace

class PostStore::Db {
public:
  explicit Db(sqlite3* db) : db_(db) {}
  ~Db() { sqlite3_close(db_); }
  Db(Db&& other) noexcept : db_(std::exchange(other.db_, nullptr)) {}
  Db& operator=(Db&&) = delete;
  Db(const Db&) = delete;

  sqlite3* get() const { return db_; }

private:
  sqlite3* db_;
};

PostStore::PostStore(std::string profile_db_path) : profile_db_path_(std::move(profile_db_path)) {
  redirectLogger("PostStore");
}

Roe<PostStore::Db> PostStore::Open() const {
  sqlite3* raw = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &raw) != SQLITE_OK) {
    sqlite3_close(raw);
    return Error("Failed to open profile.db for posts");
  }
  Db db(raw);
  sqlite3_busy_timeout(raw, 5000);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(raw, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "posts schema failed";
      sqlite3_free(err);
      return Error(message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<PostStore::Added> PostStore::AddEntry(const SignedPostEntry& entry, const PostEntry& parsed,
                                          const PostHash& entry_hash) {
  StoredPostEntry row{.author = parsed.author,
                      .lane = parsed.lane,
                      .seq = parsed.seq,
                      .entry_hash = entry_hash,
                      .kind = parsed.kind,
                      .claimed_at = parsed.claimed_at,
                      .size = entry.canonical.size() + entry.signature.size(),
                      .signed_bytes = SerializeSignedPostEntry(entry),
                      .stub = PostStub::None};
  return AddStub(row);
}

Roe<PostStore::Added> PostStore::AddStub(const StoredPostEntry& row) {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  sqlite3* raw = db.value().get();
  Stmt existing(raw, "SELECT entry_hash, stub, signed IS NULL FROM post_entries WHERE author = ? AND lane = ? AND "
                     "seq = ?;");
  if (!existing.ok()) {
    return Error("Failed to prepare post read");
  }
  existing.Text(1, row.author).Blob(2, row.lane).Int(3, static_cast<int64_t>(row.seq));
  bool fork = false;
  while (existing.Row()) {
    if (existing.ColArray<kPostHashBytes>(0) != row.entry_hash) {
      fork = true;
      continue;
    }
    // Already here. An unkept stub gains the bytes; a deleted entry stays deleted.
    const bool held_as_stub = existing.ColInt(2) != 0;
    if (held_as_stub && row.signed_bytes && static_cast<PostStub>(existing.ColInt(1)) == PostStub::Unkept) {
      Stmt fill(raw, "UPDATE post_entries SET signed = ?, stub = 0 WHERE author = ? AND lane = ? AND seq = ? AND "
                     "entry_hash = ?;");
      fill.Blob(1, *row.signed_bytes).Text(2, row.author).Blob(3, row.lane).Int(4, static_cast<int64_t>(row.seq));
      fill.Blob(5, row.entry_hash);
      if (!fill.ok() || !fill.Done()) {
        return Error("Failed to fill post stub");
      }
    }
    return Added::Duplicate;
  }
  Stmt insert(raw, "INSERT INTO post_entries (author, lane, seq, entry_hash, kind, claimed_at, size, signed, stub) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);");
  if (!insert.ok()) {
    return Error("Failed to prepare post write");
  }
  insert.Text(1, row.author).Blob(2, row.lane).Int(3, static_cast<int64_t>(row.seq)).Blob(4, row.entry_hash);
  insert.Int(5, row.kind).Int(6, row.claimed_at).Int(7, static_cast<int64_t>(row.size));
  row.signed_bytes ? insert.Blob(8, *row.signed_bytes) : insert.Null(8);
  insert.Int(9, static_cast<int64_t>(row.signed_bytes ? PostStub::None : row.stub));
  if (!insert.Done()) {
    return Error("Failed to write post");
  }
  return fork ? Added::Fork : Added::New;
}

Roe<void> PostStore::StubOut(const std::string& author, const PostLane& lane, const uint64_t seq,
                             const PostHash& entry_hash, const PostStub why) {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt update(db.value().get(),
              "UPDATE post_entries SET signed = NULL, stub = ? WHERE author = ? AND lane = ? AND seq = ? AND "
              "entry_hash = ?;");
  if (!update.ok()) {
    return Error("Failed to prepare post stub");
  }
  update.Int(1, static_cast<int64_t>(why)).Text(2, author).Blob(3, lane).Int(4, static_cast<int64_t>(seq));
  update.Blob(5, entry_hash);
  if (!update.Done()) {
    return Error("Failed to stub post");
  }
  return {};
}

Roe<std::vector<StoredPostEntry>> PostStore::At(const std::string& author, const PostLane& lane,
                                                const uint64_t seq) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  const std::string sql =
      std::string("SELECT ") + kEntryColumns + " FROM post_entries WHERE author = ? AND lane = ? AND seq = ? "
                                               "ORDER BY entry_hash;";
  Stmt select(db.value().get(), sql.c_str());
  if (!select.ok()) {
    return Error("Failed to prepare post read");
  }
  select.Text(1, author).Blob(2, lane).Int(3, static_cast<int64_t>(seq));
  std::vector<StoredPostEntry> out;
  while (select.Row()) {
    out.push_back(EntryFromRow(select));
  }
  return out;
}

Roe<std::vector<StoredPostEntry>> PostStore::Range(const std::string& author, const PostLane& lane,
                                                   const uint64_t from_seq, const size_t limit) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  const std::string sql =
      std::string("SELECT ") + kEntryColumns +
      " FROM post_entries WHERE author = ? AND lane = ? AND seq >= ? ORDER BY seq, entry_hash LIMIT ?;";
  Stmt select(db.value().get(), sql.c_str());
  if (!select.ok()) {
    return Error("Failed to prepare post range");
  }
  select.Text(1, author).Blob(2, lane).Int(3, static_cast<int64_t>(from_seq));
  select.Int(4, static_cast<int64_t>(std::min(limit, kMaxPostPageEntries)));
  std::vector<StoredPostEntry> out;
  while (select.Row()) {
    out.push_back(EntryFromRow(select));
  }
  return out;
}

Roe<std::vector<PostHeadLane>> PostStore::LanesOf(const std::string& author) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt select(db.value().get(),
              "SELECT lane, seq, MIN(entry_hash) FROM post_entries AS e WHERE author = ?1 AND seq = "
              "(SELECT MAX(seq) FROM post_entries WHERE author = ?1 AND lane = e.lane) GROUP BY lane ORDER BY lane;");
  if (!select.ok()) {
    return Error("Failed to prepare post lanes");
  }
  select.Text(1, author);
  std::vector<PostHeadLane> out;
  while (select.Row()) {
    out.push_back({.lane = select.ColArray<kPostLaneBytes>(0),
                   .seq = static_cast<uint64_t>(select.ColInt(1)),
                   .entry_hash = select.ColArray<kPostHashBytes>(2)});
  }
  return out;
}

Roe<bool> PostStore::HasFork(const std::string& author) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt select(db.value().get(),
              "SELECT 1 FROM post_entries WHERE author = ? GROUP BY lane, seq HAVING COUNT(*) > 1 LIMIT 1;");
  if (!select.ok()) {
    return Error("Failed to prepare fork check");
  }
  select.Text(1, author);
  return select.Row();
}

Roe<void> PostStore::PutHead(const SignedPostHead& head, const PostHead& parsed) {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt upsert(db.value().get(),
              "INSERT INTO post_heads (author, issued_at, canonical, signature) VALUES (?, ?, ?, ?) "
              "ON CONFLICT(author) DO UPDATE SET issued_at = excluded.issued_at, canonical = excluded.canonical, "
              "signature = excluded.signature WHERE excluded.issued_at > post_heads.issued_at;");
  if (!upsert.ok()) {
    return Error("Failed to prepare head write");
  }
  upsert.Text(1, parsed.author).Int(2, parsed.issued_at).Blob(3, head.canonical).Blob(4, head.signature);
  if (!upsert.Done()) {
    return Error("Failed to write head");
  }
  return {};
}

Roe<std::optional<SignedPostHead>> PostStore::Head(const std::string& author) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt select(db.value().get(), "SELECT canonical, signature FROM post_heads WHERE author = ?;");
  if (!select.ok()) {
    return Error("Failed to prepare head read");
  }
  select.Text(1, author);
  if (!select.Row()) {
    return std::optional<SignedPostHead>();
  }
  return std::optional<SignedPostHead>(SignedPostHead{select.ColBlob(0), select.ColBlob(1)});
}

Roe<std::optional<OwnPostLane>> PostStore::OwnLane(const std::string& device) const {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt select(db.value().get(),
              "SELECT lane, next_seq, last_hash, last_claimed_at FROM post_own_lanes WHERE device = ?;");
  if (!select.ok()) {
    return Error("Failed to prepare own lane read");
  }
  select.Text(1, device);
  if (!select.Row()) {
    return std::optional<OwnPostLane>();
  }
  return std::optional<OwnPostLane>(OwnPostLane{.device = device,
                                                .lane = select.ColArray<kPostLaneBytes>(0),
                                                .next_seq = static_cast<uint64_t>(select.ColInt(1)),
                                                .last_hash = select.ColArray<kPostHashBytes>(2),
                                                .last_claimed_at = select.ColInt(3)});
}

Roe<void> PostStore::PutOwnLane(const OwnPostLane& lane) {
  std::lock_guard lock(mutex_);
  auto db = Open();
  if (!db) {
    return db.error();
  }
  Stmt upsert(db.value().get(),
              "INSERT INTO post_own_lanes (device, lane, next_seq, last_hash, last_claimed_at) VALUES (?, ?, ?, ?, ?) "
              "ON CONFLICT(device) DO UPDATE SET lane = excluded.lane, next_seq = excluded.next_seq, "
              "last_hash = excluded.last_hash, last_claimed_at = excluded.last_claimed_at;");
  if (!upsert.ok()) {
    return Error("Failed to prepare own lane write");
  }
  upsert.Text(1, lane.device).Blob(2, lane.lane).Int(3, static_cast<int64_t>(lane.next_seq));
  upsert.Blob(4, lane.last_hash).Int(5, lane.last_claimed_at);
  if (!upsert.Done()) {
    return Error("Failed to write own lane");
  }
  return {};
}

} // namespace pbr
