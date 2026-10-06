#include "domain/messaging/ThreadLocalPrefsStore.h"

#include "common/Utilities.h"
#include "domain/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <sqlite3.h>

namespace pbr {
namespace {

class ThreadLocalPrefsStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_thread_prefs_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    prefs_ = std::make_unique<ThreadLocalPrefsStore>(store_->ProfileDbPath());
  }

  void TearDown() override {
    prefs_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ThreadLocalPrefsStore> prefs_;
};

TEST_F(ThreadLocalPrefsStoreTest, ThreadWithoutARowHasDefaults) {
  auto got = prefs_->Get("thread-1");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->thread_id, "thread-1");
  EXPECT_EQ(got->pinned_at, 0);
  EXPECT_EQ(got->muted_until, 0);
  EXPECT_FALSE(got->archived);
  auto all = prefs_->List();
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->empty());
}

TEST_F(ThreadLocalPrefsStoreTest, SetAndReadBack) {
  ThreadLocalPrefs p;
  p.thread_id = "thread-1";
  p.pinned_at = 1700000000123;
  p.muted_until = kThreadMutedForever;
  p.archived = true;
  ASSERT_TRUE(prefs_->Set(p));

  auto got = prefs_->Get("thread-1");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->pinned_at, 1700000000123);
  EXPECT_EQ(got->muted_until, kThreadMutedForever);
  EXPECT_TRUE(got->archived);

  p.pinned_at = 0; // unpin, keep the rest
  ASSERT_TRUE(prefs_->Set(p));
  got = prefs_->Get("thread-1");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->pinned_at, 0);
  EXPECT_TRUE(got->archived);

  auto all = prefs_->List();
  ASSERT_TRUE(all);
  ASSERT_EQ(all->size(), 1u);
  EXPECT_EQ(all->front().thread_id, "thread-1");
}

TEST_F(ThreadLocalPrefsStoreTest, MarkedUnreadIsStoredAndIsNotADefault) {
  ThreadLocalPrefs p;
  p.thread_id = "thread-1";
  p.marked_unread = true;
  ASSERT_TRUE(prefs_->Set(p));
  auto got = prefs_->Get("thread-1");
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->marked_unread);
  auto all = prefs_->List();
  ASSERT_TRUE(all);
  ASSERT_EQ(all->size(), 1u);
  EXPECT_TRUE(all->front().marked_unread);

  p.marked_unread = false;
  ASSERT_TRUE(prefs_->Set(p));
  all = prefs_->List();
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->empty());
}

TEST_F(ThreadLocalPrefsStoreTest, ColumnAddedLaterIsAddedToAnExistingTable) {
  // A table created before `marked_unread` existed (our own dev devices have one) gains the column.
  const std::string db_path = store_->ProfileDbPath();
  prefs_.reset();
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db,
                         "DROP TABLE IF EXISTS thread_local_prefs;"
                         "CREATE TABLE thread_local_prefs (thread_id TEXT PRIMARY KEY, pinned_at INTEGER NOT NULL "
                         "DEFAULT 0, muted_until INTEGER NOT NULL DEFAULT 0, archived INTEGER NOT NULL DEFAULT 0);"
                         "INSERT INTO thread_local_prefs (thread_id, pinned_at) VALUES ('thread-old', 7);",
                         nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(db);

  prefs_ = std::make_unique<ThreadLocalPrefsStore>(db_path);
  auto old = prefs_->Get("thread-old");
  ASSERT_TRUE(old);
  EXPECT_EQ(old->pinned_at, 7);
  EXPECT_FALSE(old->marked_unread);
  ThreadLocalPrefs p = *old;
  p.marked_unread = true;
  ASSERT_TRUE(prefs_->Set(p));
  auto got = prefs_->Get("thread-old");
  ASSERT_TRUE(got);
  EXPECT_TRUE(got->marked_unread);
  EXPECT_EQ(got->pinned_at, 7);
}

TEST_F(ThreadLocalPrefsStoreTest, AllDefaultsLeaveNoRow) {
  ThreadLocalPrefs p;
  p.thread_id = "thread-1";
  p.archived = true;
  ASSERT_TRUE(prefs_->Set(p));
  p.archived = false;
  ASSERT_TRUE(prefs_->Set(p));
  auto all = prefs_->List();
  ASSERT_TRUE(all);
  EXPECT_TRUE(all->empty());
}

TEST_F(ThreadLocalPrefsStoreTest, DeleteRemovesTheRow) {
  ThreadLocalPrefs p;
  p.thread_id = "thread-1";
  p.pinned_at = 5;
  ASSERT_TRUE(prefs_->Set(p));
  ASSERT_TRUE(prefs_->Delete("thread-1"));
  ASSERT_TRUE(prefs_->Delete("thread-1")); // deleting nothing is fine
  auto got = prefs_->Get("thread-1");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->pinned_at, 0);
}

TEST_F(ThreadLocalPrefsStoreTest, TableIsAddedToAnExistingProfileDbWithoutTouchingItsVersion) {
  // A profile.db written before this table existed: drop the table, reopen, and the thread store still
  // opens (same user_version) while the prefs store creates what it needs.
  Thread thread;
  thread.id = "thread-old";
  thread.kind = ThreadKind::Ai;
  thread.title = "Old";
  thread.updated_at = 1;
  ASSERT_TRUE(store_->UpsertThread(thread));
  const std::string db_path = store_->ProfileDbPath();
  prefs_.reset();
  store_.reset();

  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_exec(db, "DROP TABLE IF EXISTS thread_local_prefs;", nullptr, nullptr, nullptr), SQLITE_OK);
  int version_before = -1;
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  version_before = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  sqlite3_close(db);

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  auto threads = store_->ListThreads();
  ASSERT_TRUE(threads);
  ASSERT_EQ(threads->size(), 1u);
  prefs_ = std::make_unique<ThreadLocalPrefsStore>(db_path);
  ThreadLocalPrefs p;
  p.thread_id = "thread-old";
  p.pinned_at = 9;
  ASSERT_TRUE(prefs_->Set(p));
  auto got = prefs_->Get("thread-old");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->pinned_at, 9);

  ASSERT_EQ(sqlite3_open(db_path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr), SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), version_before);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

} // namespace
} // namespace pbr
