// The Groups page (Contacts / New chat → Groups): the groups this account is in now, each with its chat. A linked
// device's first account sync recreates the account's groups (GroupMembershipWorkflow::RestoreGroup: the roster rows in
// Checking, a chat with no message yet); those are listed, and their chat is a row the Chats list shows.
#include "lib/messaging/LiveGroups.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr char kMe[] = "account:me";

class LiveGroupsTest : public ::testing::Test {
protected:
  void SetUp() override { roster_ = std::make_unique<InMemoryGroupRosterStore>(threads_); }

  /** A group as RestoreGroup (or a join) leaves it: metadata, its members, and its chat. */
  std::string AddGroup(const std::string& group_id, const GroupSyncState state, const std::vector<std::string>& members,
                       const bool with_chat = true) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = members.empty() ? std::string("account:owner") : members.front();
    metadata.title = "Group " + group_id;
    metadata.sync_state = state;
    EXPECT_TRUE(roster_->UpsertMetadata(metadata));
    for (const std::string& identity : members) {
      GroupRosterMember member;
      member.member_identity = identity;
      EXPECT_TRUE(roster_->UpsertMember(group_id, member));
    }
    if (!with_chat) {
      return {};
    }
    auto thread = threads_.FindOrCreateGroupThread(group_id, metadata.title, {});
    EXPECT_TRUE(thread);
    return thread ? thread->id : std::string();
  }

  std::vector<std::string> ListedIds() {
    auto live = ListLiveGroups(*roster_, kMe);
    EXPECT_TRUE(live) << (live ? "" : live.error().message);
    std::vector<std::string> ids;
    if (live) {
      for (const LiveGroup& group : *live) {
        ids.push_back(group.group_id);
      }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  InMemoryDeliveryStore threads_;
  std::unique_ptr<InMemoryGroupRosterStore> roster_;
};

TEST_F(LiveGroupsTest, OnlyTheGroupsThisAccountIsInNowAreListed) {
  AddGroup("active", GroupSyncState::Active, {"account:owner", kMe, "account:bob"});
  AddGroup("restored", GroupSyncState::Checking, {"account:owner", kMe}); // a linked device's first sync
  AddGroup("left", GroupSyncState::Active, {"account:owner", "account:bob"});
  AddGroup("removed", GroupSyncState::Removed, {"account:owner", kMe});
  AddGroup("dissolved", GroupSyncState::Dissolved, {"account:owner", kMe});
  AddGroup("no_chat", GroupSyncState::Active, {"account:owner", kMe}, /*with_chat=*/false);
  EXPECT_EQ(ListedIds(), (std::vector<std::string>{"active", "restored"}));
}

TEST_F(LiveGroupsTest, EachRowCarriesItsChatAndMemberCount) {
  const std::string thread_id = AddGroup("g1", GroupSyncState::Active, {"account:owner", kMe, "account:bob"});
  auto live = ListLiveGroups(*roster_, kMe);
  ASSERT_TRUE(live);
  ASSERT_EQ(live->size(), 1u);
  EXPECT_EQ(live->front().thread_id, thread_id);
  EXPECT_EQ(live->front().member_count, 3u);
}

TEST_F(LiveGroupsTest, NoAccountYetListsNothing) {
  AddGroup("g1", GroupSyncState::Active, {"account:owner", kMe});
  auto live = ListLiveGroups(*roster_, "");
  ASSERT_TRUE(live);
  EXPECT_TRUE(live->empty());
}

// A recreated group has no message yet: its chat is still a row of the thread list, unread 0, at the time it was made,
// listed — what the Chats list builds its rows from (ChatPresenter::Refresh hides only archived chats and unlisted AI
// ones).
TEST_F(LiveGroupsTest, ARestoredGroupsChatIsAListedRowWithoutMessages) {
  const std::string thread_id = AddGroup("restored", GroupSyncState::Checking, {"account:owner", kMe});
  auto listed = threads_.ListThreads();
  ASSERT_TRUE(listed);
  const auto row = std::find_if(listed->begin(), listed->end(), [&](const Thread& t) { return t.id == thread_id; });
  ASSERT_NE(row, listed->end());
  EXPECT_EQ(row->kind, ThreadKind::Group);
  EXPECT_TRUE(row->listed);
  EXPECT_EQ(row->unread_count, 0);
  EXPECT_GT(row->updated_at, 0);
  EXPECT_TRUE(row->preview.empty()) << "the list says \"No messages yet\" for it";
}

} // namespace
} // namespace pbr
