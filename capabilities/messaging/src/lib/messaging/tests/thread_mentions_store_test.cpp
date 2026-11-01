// The unread-mention marker (group-chat G015): per message, dropped with the message, the opened thread or the
// deleted one.
#include "lib/messaging/ThreadMentionsStore.h"

#include "common/Utilities.h"
#include "lib/messaging/SqliteThreadStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

class ThreadMentionsStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_thread_mentions_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    mentions_ = std::make_unique<ThreadMentionsStore>(store_->ProfileDbPath());
  }

  void TearDown() override {
    mentions_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ThreadMentionsStore> mentions_;
};

TEST_F(ThreadMentionsStoreTest, TheOldestUnreadMentionIsWhereTheChatOpens) {
  EXPECT_TRUE(mentions_->Threads()->empty());
  EXPECT_FALSE(mentions_->Oldest("hike").value());
  ASSERT_TRUE(mentions_->Add("hike", "m-later", 2000));
  ASSERT_TRUE(mentions_->Add("hike", "m-first", 1000));
  ASSERT_TRUE(mentions_->Add("hike", "m-first", 1000)) << "the same message twice: one row";
  ASSERT_TRUE(mentions_->Add("books", "m-x", 1500));
  EXPECT_EQ(*mentions_->Threads(), (std::set<std::string>{"books", "hike"}));
  EXPECT_EQ(mentions_->Oldest("hike").value(), std::optional<std::string>("m-first"));
}

TEST_F(ThreadMentionsStoreTest, ADeletedMessageOrAnOpenedChatLeavesNoMarker) {
  ASSERT_TRUE(mentions_->Add("hike", "m-1", 1000));
  ASSERT_TRUE(mentions_->Add("hike", "m-2", 2000));
  ASSERT_TRUE(mentions_->DropMessage("hike", "m-1"));
  EXPECT_EQ(mentions_->Oldest("hike").value(), std::optional<std::string>("m-2"));
  ASSERT_TRUE(mentions_->DropThread("hike"));
  EXPECT_TRUE(mentions_->Threads()->empty());
}

} // namespace
} // namespace pbr
