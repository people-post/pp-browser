// A 1:1 or group chat cleared or deleted here: the ids of its messages are kept per sender (profile.db
// `removed_message_ids`, chat-storage D107), so a copy the relay still holds is not stored again. Pruned after the
// relay's window, gone with the profile.
#include "lib/storage/SqliteThreadSchema.h"
#include "lib/storage/SqliteThreadStore.h"

#include "UnlockedThreadStore.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace pbr {
namespace {

class RemovedMessagesStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_removed_messages_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    store_ = test::MakeUnlockedThreadStore(dir_.string());
  }
  void TearDown() override {
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  Thread PublicChat(const std::string& peer) {
    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = peer;
    target.channel = ThreadChannel::E2ePublic;
    auto thread = store_->FindOrCreateDirectThread(target, "", peer);
    EXPECT_TRUE(thread);
    return *thread;
  }

  ThreadMessage Received(const Thread& thread, const std::string& id, const int64_t timestamp = util::NowUnixMs(),
                         const std::string& sender = "peer-contact") {
    ThreadMessage message;
    message.id = id;
    message.thread_id = thread.id;
    message.sender_contact_id = sender; // a 1:1 row names the peer by its contact id, a group's by its Account ID
    message.text = "hello " + id;
    message.timestamp = timestamp;
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    return message;
  }

  bool Removed(const std::string& sender, const std::string& id) {
    auto removed = store_->WasMessageRemoved(sender, id);
    EXPECT_TRUE(removed);
    return removed && *removed;
  }

  /** Runs `sql` on profile.db beside the store (a second connection, as other profile.db stores open it). */
  void Exec(const std::string& sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(SqliteThreadProfileDbFile(dir_.string()).c_str(), &db), SQLITE_OK);
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    const std::string message = err ? err : "";
    sqlite3_free(err);
    sqlite3_close(db);
    ASSERT_EQ(rc, SQLITE_OK) << message;
  }

  int64_t ProfileUserVersion() {
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(SqliteThreadProfileDbFile(dir_.string()).c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    int64_t version = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
      version = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return version;
  }

  std::filesystem::path dir_;
  std::unique_ptr<SqliteThreadStore> store_;
};

TEST_F(RemovedMessagesStoreTest, AClearedChatsIdsAreRememberedAndNotStoredAgainWhileANewMessageIs) {
  const Thread chat = PublicChat("peer-a");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "m1")));
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "m2")));
  EXPECT_FALSE(Removed("peer-a", "m1")) << "a message that is still there is not removed";

  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("peer-a", "m1"));
  EXPECT_TRUE(Removed("peer-a", "m2"));
  auto has = store_->HasMessageId(chat.id, "m1");
  ASSERT_TRUE(has);
  EXPECT_FALSE(*has) << "HasMessageId still answers for rows only";

  EXPECT_FALSE(store_->AppendMessage(Received(chat, "m1"))) << "a copy that comes again is refused";
  EXPECT_FALSE(store_->UpsertMessage(Received(chat, "m2")));
  EXPECT_TRUE(store_->AppendMessage(Received(chat, "m3"))) << "a message after the clear is stored";
  auto page = store_->GetMessagesPage(chat.id, std::nullopt, 10);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->size(), 1u);
  EXPECT_EQ(page->front().id, "m3");
}

TEST_F(RemovedMessagesStoreTest, ADeletedChatsIdsAreRememberedAGroupsToo) {
  const Thread chat = PublicChat("peer-b");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "d1")));
  auto group = store_->FindOrCreateGroupThread("g1", "Group", {});
  ASSERT_TRUE(group);
  ASSERT_TRUE(store_->AppendMessage(Received(*group, "g-msg", util::NowUnixMs(), "member-account")));

  ASSERT_TRUE(store_->DeleteThread(chat.id));
  ASSERT_TRUE(store_->DeleteThread(group->id));
  EXPECT_TRUE(Removed("peer-b", "d1"));
  EXPECT_TRUE(Removed("member-account", "g-msg"));
}

TEST_F(RemovedMessagesStoreTest, AnAiChatsClearRemembersNothing) {
  Thread ai;
  ai.id = "ai-thread";
  ai.kind = ThreadKind::Ai;
  ai.title = "AI";
  ai.updated_at = 1;
  ASSERT_TRUE(store_->UpsertThread(ai));
  ThreadMessage message;
  message.id = "ai-1";
  message.thread_id = ai.id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = "hi";
  message.timestamp = util::NowUnixMs();
  ASSERT_TRUE(store_->AppendMessage(message));
  ASSERT_TRUE(store_->ClearMessages(ai.id, ClearMessagesOptions{}));
  EXPECT_FALSE(Removed(kLocalSelfContactId, "ai-1")) << "nobody sends an AI chat's messages again";
}

TEST_F(RemovedMessagesStoreTest, RowsOlderThanAnyRelayRecordAreNotRemembered) {
  const Thread chat = PublicChat("peer-c");
  const int64_t old = util::NowUnixMs() - kRemovedMessageRowHorizonMs - 24LL * 60 * 60 * 1000;
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "ancient", old)));
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "recent")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_FALSE(Removed("peer-c", "ancient"));
  EXPECT_TRUE(Removed("peer-c", "recent"));
}

TEST_F(RemovedMessagesStoreTest, IdsPastTheRelaysWindowArePrunedOnOpenAndAtTheNextRemoval) {
  const Thread chat = PublicChat("peer-d");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "kept")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  const int64_t past_window = util::NowUnixMs() - kRemovedMessageIdKeepMs - 60'000;
  Exec("INSERT INTO removed_message_ids (sender, message_id, removed_at) VALUES ('peer-d', 'expired-1', " +
       std::to_string(past_window) + ");");
  EXPECT_TRUE(Removed("peer-d", "expired-1"));

  store_.reset(); // a restart
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  EXPECT_FALSE(Removed("peer-d", "expired-1")) << "pruned when profile.db opens";
  EXPECT_TRUE(Removed("peer-d", "kept"));

  Exec("INSERT INTO removed_message_ids (sender, message_id, removed_at) VALUES ('peer-d', 'expired-2', " +
       std::to_string(past_window) + ");");
  const Thread other = PublicChat("peer-e");
  ASSERT_TRUE(store_->AppendMessage(Received(other, "x1")));
  ASSERT_TRUE(store_->DeleteThread(other.id));
  EXPECT_FALSE(Removed("peer-d", "expired-2")) << "pruned at the next removal";
  EXPECT_TRUE(Removed("peer-e", "x1"));
}

TEST_F(RemovedMessagesStoreTest, AProfileResetTakesThemAway) {
  const Thread chat = PublicChat("peer-f");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "r1")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("peer-f", "r1"));
  // Reset my profile (Application::ResetActiveProfile): the profile directory goes, the stores open anew.
  store_.reset();
  std::filesystem::remove_all(dir_);
  std::filesystem::create_directories(dir_);
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  EXPECT_FALSE(Removed("peer-f", "r1"));
}

TEST_F(RemovedMessagesStoreTest, AnExistingProfileGetsTheTableWithoutAVersionStep) {
  ASSERT_TRUE(store_->ListThreads()); // opened at this build's schema
  store_.reset();
  Exec("DROP TABLE removed_message_ids;"); // a file from before the table
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  const Thread chat = PublicChat("peer-g");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "n1")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("peer-g", "n1"));
  EXPECT_EQ(ProfileUserVersion(), kSqliteThreadProfileUserVersion) << "an older app opens the file as before";
}

TEST_F(RemovedMessagesStoreTest, AnIdCountsOnlyForItsSender) {
  const Thread chat = PublicChat("peer-h");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "same-id")));
  ThreadMessage ours = Received(chat, "ours-1", util::NowUnixMs(), kLocalSelfContactId);
  ASSERT_TRUE(store_->AppendMessage(ours));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("peer-h", "same-id")) << "the peer's row: kept under the peer's Account ID, not its contact id";
  EXPECT_TRUE(Removed(kLocalSelfContactId, "ours-1")) << "ours: kept as ours (another device's copy maps to it)";
  EXPECT_FALSE(Removed("peer-i", "same-id")) << "another contact's message with that id is not dropped";
  EXPECT_FALSE(Removed("peer-h", "ours-1"));

  const Thread other = PublicChat("peer-i");
  EXPECT_TRUE(store_->AppendMessage(Received(other, "same-id"))) << "stored: it is another sender's";
  EXPECT_FALSE(store_->AppendMessage(Received(chat, "same-id"))) << "the cleared chat's own copy is still refused";
}

TEST_F(RemovedMessagesStoreTest, TheGlobalListFromBeforeIsDroppedOnOpen) {
  ASSERT_TRUE(store_->ListThreads());
  store_.reset();
  Exec(
      "CREATE TABLE removed_messages (message_id TEXT PRIMARY KEY, removed_at INTEGER NOT NULL);"
      "INSERT INTO removed_messages VALUES ('old-1', 1);"); // a file written by #724's build
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  ASSERT_TRUE(store_->ListThreads());
  sqlite3* db = nullptr;
  ASSERT_EQ(sqlite3_open(SqliteThreadProfileDbFile(dir_.string()).c_str(), &db), SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(
      sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'removed_messages';", -1, &stmt, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_EQ(sqlite3_column_int(stmt, 0), 0);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  EXPECT_EQ(ProfileUserVersion(), kSqliteThreadProfileUserVersion);
}

} // namespace
} // namespace pbr
