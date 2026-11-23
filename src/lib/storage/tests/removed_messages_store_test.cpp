// A 1:1 or group chat cleared or deleted here: the ids of its messages are kept (profile.db `removed_messages`), so a
// copy the relay still holds is not stored again. Ids only, pruned after the relay's window, gone with the profile.
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

  ThreadMessage Received(const Thread& thread, const std::string& id, const int64_t timestamp = util::NowUnixMs()) {
    ThreadMessage message;
    message.id = id;
    message.thread_id = thread.id;
    message.sender_contact_id = "peer";
    message.text = "hello " + id;
    message.timestamp = timestamp;
    message.delivery = MessageDelivery::Relayed;
    message.relay_visible = true;
    return message;
  }

  bool Removed(const std::string& id) {
    auto removed = store_->WasMessageRemoved(id);
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
  EXPECT_FALSE(Removed("m1")) << "a message that is still there is not removed";

  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("m1"));
  EXPECT_TRUE(Removed("m2"));
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
  ASSERT_TRUE(store_->AppendMessage(Received(*group, "g-msg")));

  ASSERT_TRUE(store_->DeleteThread(chat.id));
  ASSERT_TRUE(store_->DeleteThread(group->id));
  EXPECT_TRUE(Removed("d1"));
  EXPECT_TRUE(Removed("g-msg"));
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
  EXPECT_FALSE(Removed("ai-1")) << "nobody sends an AI chat's messages again";
}

TEST_F(RemovedMessagesStoreTest, RowsOlderThanAnyRelayRecordAreNotRemembered) {
  const Thread chat = PublicChat("peer-c");
  const int64_t old = util::NowUnixMs() - kRemovedMessageRowHorizonMs - 24LL * 60 * 60 * 1000;
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "ancient", old)));
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "recent")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_FALSE(Removed("ancient"));
  EXPECT_TRUE(Removed("recent"));
}

TEST_F(RemovedMessagesStoreTest, IdsPastTheRelaysWindowArePrunedOnOpenAndAtTheNextRemoval) {
  const Thread chat = PublicChat("peer-d");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "kept")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  const int64_t past_window = util::NowUnixMs() - kRemovedMessageIdKeepMs - 60'000;
  Exec("INSERT INTO removed_messages (message_id, removed_at) VALUES ('expired-1', " + std::to_string(past_window) +
       ");");
  EXPECT_TRUE(Removed("expired-1"));

  store_.reset(); // a restart
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  EXPECT_FALSE(Removed("expired-1")) << "pruned when profile.db opens";
  EXPECT_TRUE(Removed("kept"));

  Exec("INSERT INTO removed_messages (message_id, removed_at) VALUES ('expired-2', " + std::to_string(past_window) +
       ");");
  const Thread other = PublicChat("peer-e");
  ASSERT_TRUE(store_->AppendMessage(Received(other, "x1")));
  ASSERT_TRUE(store_->DeleteThread(other.id));
  EXPECT_FALSE(Removed("expired-2")) << "pruned at the next removal";
  EXPECT_TRUE(Removed("x1"));
}

TEST_F(RemovedMessagesStoreTest, AProfileResetTakesThemAway) {
  const Thread chat = PublicChat("peer-f");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "r1")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("r1"));
  // Reset my profile (Application::ResetActiveProfile): the profile directory goes, the stores open anew.
  store_.reset();
  std::filesystem::remove_all(dir_);
  std::filesystem::create_directories(dir_);
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  EXPECT_FALSE(Removed("r1"));
}

TEST_F(RemovedMessagesStoreTest, AnExistingProfileGetsTheTableWithoutAVersionStep) {
  ASSERT_TRUE(store_->ListThreads()); // opened at this build's schema
  store_.reset();
  Exec("DROP TABLE removed_messages;"); // a file from before the table
  store_ = test::MakeUnlockedThreadStore(dir_.string());
  const Thread chat = PublicChat("peer-g");
  ASSERT_TRUE(store_->AppendMessage(Received(chat, "n1")));
  ASSERT_TRUE(store_->ClearMessages(chat.id, ClearMessagesOptions{}));
  EXPECT_TRUE(Removed("n1"));
  EXPECT_EQ(ProfileUserVersion(), kSqliteThreadProfileUserVersion) << "an older app opens the file as before";
}

} // namespace
} // namespace pbr
