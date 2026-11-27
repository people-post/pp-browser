#include "domain/relay/RelayStore.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "common/PbrCompat.h"

using namespace pbr;
using relay::MailRecord;
using relay::RelayStore;

namespace {

constexpr int64_t kDay = 24LL * 60 * 60 * 1000;

class RelayStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp-relay-store-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  }
  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(dir_);
  }

  RelayStore& Open(relay::RelayLimits limits = {}) {
    store_.reset();
    auto opened = RelayStore::Open(dir_.string(), limits, [this] { return now_; });
    EXPECT_TRUE(static_cast<bool>(opened)) << (opened ? "" : opened.error().message);
    store_ = std::move(*opened);
    return *store_;
  }

  static MailRecord Record(const std::string& sender, const std::string& stream, uint64_t index_key, size_t size = 10) {
    MailRecord r;
    r.sender_account_id = sender;
    r.stream_id = stream;
    r.index_key = index_key;
    r.body.assign(size, 0x5a);
    return r;
  }

  std::filesystem::path dir_;
  int64_t now_ = 1'764'201'600'000;
  std::unique_ptr<RelayStore> store_;
};

} // namespace

TEST_F(RelayStoreTest, APutIsPolledInOrderAndTheCursorMovesOn) {
  auto& store = Open();
  for (uint64_t i = 1; i <= 3; ++i) {
    const auto put = store.Put("account:bob", Record("account:alice", "s1", i));
    ASSERT_TRUE(static_cast<bool>(put));
    EXPECT_EQ(put->outcome, RelayStore::PutOutcome::Stored);
    EXPECT_EQ(put->seq, i);
  }
  auto page = store.Poll("account:bob", 0, 2, 1 << 20);
  ASSERT_TRUE(static_cast<bool>(page));
  ASSERT_EQ(page->records.size(), 2u);
  EXPECT_EQ(page->records[0].index_key, 1u);
  EXPECT_EQ(page->records[0].created_at_ms, now_);
  EXPECT_EQ(page->records[0].expires_at_ms, now_ + 7 * kDay);
  EXPECT_TRUE(page->more);
  page = store.Poll("account:bob", page->next_after, 2, 1 << 20);
  ASSERT_EQ(page->records.size(), 1u);
  EXPECT_EQ(page->records[0].seq, 3u);
  EXPECT_FALSE(page->more);
  EXPECT_TRUE(store.Poll("account:carol", 0, 10, 1 << 20)->records.empty()) << "mailboxes are apart";
}

TEST_F(RelayStoreTest, APageStopsAfterTheRecordThatCrossesItsBytes) {
  auto& store = Open();
  for (uint64_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:alice", "s1", i, 100))));
  }
  const auto page = store.Poll("account:bob", 0, 10, 150);
  ASSERT_EQ(page->records.size(), 2u);
  EXPECT_TRUE(page->more);
}

TEST_F(RelayStoreTest, ALiveKeyIsStoredOnceAndAnExpiredOneAgain) {
  auto& store = Open();
  ASSERT_EQ(store.Put("account:bob", Record("account:alice", "s1", 7), 60'000)->outcome,
            RelayStore::PutOutcome::Stored);
  EXPECT_EQ(store.Put("account:bob", Record("account:alice", "s1", 7))->outcome, RelayStore::PutOutcome::Duplicate);
  EXPECT_EQ(store.Put("account:bob", Record("account:alice", "s2", 7))->outcome, RelayStore::PutOutcome::Stored)
      << "another stream";
  now_ += 61'000;
  EXPECT_EQ(store.Put("account:bob", Record("account:alice", "s1", 7))->outcome, RelayStore::PutOutcome::Stored);
}

TEST_F(RelayStoreTest, TheSendersTtlAndUrgentShortenRetentionNeverLengthen) {
  auto& store = Open();
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 1), 1000)));
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 2), 3600'000)));
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 3), 30 * kDay)));
  MailRecord urgent = Record("account:a", "s", 4);
  urgent.urgent = true;
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", urgent)));
  const auto page = store.Poll("account:bob", 0, 10, 1 << 20);
  ASSERT_EQ(page->records.size(), 4u);
  EXPECT_EQ(page->records[0].expires_at_ms - now_, RelayStore::kMinTtlMs) << "at least a minute";
  EXPECT_EQ(page->records[1].expires_at_ms - now_, 3600'000);
  EXPECT_EQ(page->records[2].expires_at_ms - now_, 7 * kDay) << "never past the relay's retention";
  EXPECT_EQ(page->records[3].expires_at_ms - now_, RelayStore::kUrgentRetentionMs);
}

// NR007: past a limit the relay takes nothing new (busy); it never drops the oldest.
TEST_F(RelayStoreTest, AFullMailboxTakesNothingNew) {
  relay::RelayLimits limits;
  limits.max_records = 2;
  limits.max_bytes = 250;
  auto& store = Open(limits);
  ASSERT_EQ(store.Put("account:bob", Record("account:a", "s", 1, 100))->outcome, RelayStore::PutOutcome::Stored);
  EXPECT_EQ(store.Put("account:bob", Record("account:a", "s", 2, 200))->outcome, RelayStore::PutOutcome::FullBytes);
  ASSERT_EQ(store.Put("account:bob", Record("account:a", "s", 3, 100))->outcome, RelayStore::PutOutcome::Stored);
  EXPECT_EQ(store.Put("account:bob", Record("account:a", "s", 4, 1))->outcome, RelayStore::PutOutcome::FullRecords);
  EXPECT_EQ(store.Poll("account:bob", 0, 10, 1 << 20)->records.front().index_key, 1u) << "the oldest stays";
  EXPECT_EQ(store.UsageOf("account:bob")->records, 2);
  EXPECT_EQ(store.UsageOf("account:bob")->bytes, 200);
  EXPECT_EQ(store.Put("account:carol", Record("account:a", "s", 1, 100))->outcome, RelayStore::PutOutcome::Stored)
      << "limits are per account";
}

TEST_F(RelayStoreTest, StreamHistoryByIndexKeyBothWays) {
  auto& store = Open();
  for (uint64_t i = 1; i <= 5; ++i) {
    ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:alice", "s1", i * 10))));
  }
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:eve", "s1", 25))));
  RelayStore::StreamQuery q;
  q.sender_account_id = "account:alice";
  q.stream_id = "s1";
  q.min_index_key = 20;
  q.max_index_key = 40;
  auto page = store.Stream("account:bob", q);
  ASSERT_EQ(page->records.size(), 3u);
  EXPECT_EQ(page->records[0].index_key, 20u);
  EXPECT_FALSE(page->more);
  q.descending = true;
  q.limit = 2;
  page = store.Stream("account:bob", q);
  ASSERT_EQ(page->records.size(), 2u);
  EXPECT_EQ(page->records[0].index_key, 40u);
  EXPECT_TRUE(page->more);
}

TEST_F(RelayStoreTest, ClearAndDeleteKeepTheMailboxOrder) {
  auto& store = Open();
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 1))));
  now_ += 1000;
  ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 2))));
  EXPECT_EQ(*store.ClearBefore("account:bob", now_), 1);
  EXPECT_EQ(*store.DeleteMailbox("account:bob"), 1);
  const auto put = store.Put("account:bob", Record("account:a", "s", 3));
  EXPECT_EQ(put->seq, 3u) << "a device's cursor never points past a reused seq";
}

// NR001's data-folder rule 2: a relay back after a build without it serves nothing past its time.
TEST_F(RelayStoreTest, OpeningExpiresWhatIsDue) {
  {
    auto& store = Open();
    ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 1), 60'000)));
    ASSERT_TRUE(static_cast<bool>(store.Put("account:bob", Record("account:a", "s", 2))));
  }
  now_ += 2 * 60'000;
  auto& store = Open();
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open((dir_ / RelayStore::kFileName).string().c_str(), &db), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM records", -1, &stmt, nullptr);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int64(stmt, 0), 1) << "the expired record is gone from disk";
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  EXPECT_EQ(store.Poll("account:bob", 0, 10, 1 << 20)->records.size(), 1u);
}

// NR001's data-folder rule 2: a store from a newer build is refused and left as it is.
TEST_F(RelayStoreTest, ANewerSchemaIsRefusedUntouched) {
  Open();
  store_.reset();
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open((dir_ / RelayStore::kFileName).string().c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version=2", nullptr, nullptr, nullptr), SQLITE_OK);
  sqlite3_close(db);
  const auto opened = RelayStore::Open(dir_.string(), {});
  ASSERT_FALSE(static_cast<bool>(opened));
  EXPECT_NE(opened.error().message.find("from a newer pp-node (schema 2, this one 1)"), std::string::npos);
  ASSERT_EQ(sqlite3_open((dir_ / RelayStore::kFileName).string().c_str(), &db), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, nullptr);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

#if !defined(_WIN32)
TEST_F(RelayStoreTest, ItsFolderAndFileArePrivate) {
  Open();
  namespace fs = std::filesystem;
  EXPECT_EQ(fs::status(dir_).permissions() & fs::perms::all, fs::perms::owner_all);
  EXPECT_EQ(fs::status(dir_ / RelayStore::kFileName).permissions() & fs::perms::all,
            fs::perms::owner_read | fs::perms::owner_write);
}
#endif
