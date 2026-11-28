#pragma once

#include "common/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr::relay {

/** What the store holds per account (projects/node-relay NR007; the settings' values). */
struct RelayLimits {
  int64_t retention_days = 7;
  int64_t max_records = 1000;
  int64_t max_bytes = 16 * 1024 * 1024;
};

/**
 * One record in an account's mailbox: what the relay protocol's `send` deposits and `poll_inbox` returns (pp-specs
 * contracts/relay/PROTOCOL.md). The body is the sealed record, opaque here.
 */
struct MailRecord {
  /** The mailbox's own order, never reused; `poll_inbox`'s cursor is the last one handled. */
  uint64_t seq = 0;
  std::string sender_account_id;
  std::string stream_id;
  uint64_t index_key = 0;
  std::vector<uint8_t> body;
  bool urgent = false;
  int64_t created_at_ms = 0;
  int64_t expires_at_ms = 0;
};

/**
 * The message relay's store (projects/node-relay NR3): every account's mailbox in one SQLite file,
 * `<profile dir>/relay/relay.db` (0600, its folder 0700). Metadata (who sent to whom, streams, times) is plain, as in
 * the HTTPS relay's database; bodies are sealed end to end (Hongwei, 2026-11-27; the privacy statement says what an
 * operator's disk holds).
 *
 * NR001's data-folder rules: the file carries its schema version and a newer one is refused, untouched; opening it
 * expires what is past its time first, so a relay back after a build without it serves nothing stale.
 *
 * It mirrors the HTTPS relay's semantics: retention as the sender's `ttl_s` and `urgent` shorten it; a live
 * (sender, stream, index key) is stored once; `clear` by creation time. Unlike it, a full mailbox takes nothing new
 * (NR007: `busy`) rather than dropping the oldest. Thread-safe.
 */
class RelayStore {
public:
  static constexpr int kSchemaVersion = 1; // free to change until pp-node-plus's first release (NR5)
  static constexpr int64_t kUrgentRetentionMs = 2 * 60 * 1000;
  static constexpr int64_t kMinTtlMs = 60 * 1000;
  static constexpr const char* kFileName = "relay.db";

  /** Epoch milliseconds. */
  using Clock = std::function<int64_t()>;

  /** Opens (or creates) `<dir>/relay.db`, creating `dir`; refuses a newer schema; expires what is due. */
  static Roe<std::unique_ptr<RelayStore>> Open(const std::string& dir, RelayLimits limits, Clock clock = {});
  ~RelayStore();
  RelayStore(const RelayStore&) = delete;
  RelayStore& operator=(const RelayStore&) = delete;

  enum class PutOutcome {
    Stored,
    /** A live record of this (sender, stream, index key) is there already: the send succeeds, nothing is stored. */
    Duplicate,
    /** The mailbox holds max_records live records. */
    FullRecords,
    /** The body would take the mailbox past max_bytes. */
    FullBytes,
  };
  struct PutResult {
    PutOutcome outcome = PutOutcome::Stored;
    /** The new record's seq when Stored. */
    uint64_t seq = 0;
  };
  /** Deposits `record` (its seq and times are the store's) for `recipient`. `ttl_ms` 0: none asked. */
  Roe<PutResult> Put(const std::string& recipient, const MailRecord& record, int64_t ttl_ms = 0);

  struct Page {
    std::vector<MailRecord> records;
    /** The last seq handled: the next poll's `after`. Unchanged when the page is empty. */
    uint64_t next_after = 0;
    bool more = false;
  };
  /** Live records after `after_seq`, oldest first, at most `max_records`; stops after the one that crosses `max_bytes`. */
  Roe<Page> Poll(const std::string& recipient, uint64_t after_seq, size_t max_records, size_t max_bytes);

  struct StreamQuery {
    std::string sender_account_id;
    std::string stream_id;
    std::optional<uint64_t> min_index_key;
    std::optional<uint64_t> max_index_key;
    size_t limit = 50;
    bool descending = false;
  };
  struct StreamPage {
    std::vector<MailRecord> records;
    bool more = false;
  };
  /** One sender's stream in `recipient`'s mailbox, by index key (`stream_history`). */
  Roe<StreamPage> Stream(const std::string& recipient, const StreamQuery& query);

  /** Deletes `recipient`'s records created before `before_ms` (`clear_inbox`); how many. */
  Roe<int64_t> ClearBefore(const std::string& recipient, int64_t before_ms);
  /** Deletes `recipient`'s whole mailbox (the owner's revoke with delete_mail); how many records. */
  Roe<int64_t> DeleteMailbox(const std::string& recipient);
  /** Deletes every record past its time; how many. Open does it; NR3 runs it now and then too. */
  Roe<int64_t> Expire();

  struct Usage {
    int64_t records = 0;
    int64_t bytes = 0;
  };
  /** What `recipient`'s live records take. */
  Roe<Usage> UsageOf(const std::string& recipient);

  const RelayLimits& Limits() const { return limits_; }
  const std::string& Path() const { return path_; }

private:
  RelayStore(sqlite3* db, std::string path, RelayLimits limits, Clock clock);
  Roe<Usage> UsageLocked(const std::string& recipient, int64_t now);
  int64_t Now() const;

  mutable std::mutex mu_;
  sqlite3* db_ = nullptr;
  std::string path_;
  RelayLimits limits_;
  Clock clock_;
};

} // namespace pbr::relay
