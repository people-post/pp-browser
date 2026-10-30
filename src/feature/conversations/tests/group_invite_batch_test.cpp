#include "feature/conversations/GroupInviteBatch.h"

#include <gtest/gtest.h>

using namespace pbr;

TEST(GroupInviteBatchTest, TheFiftiethMemberIsTheLastOne) {
  EXPECT_TRUE(CheckGroupHasRoom(0));
  EXPECT_TRUE(CheckGroupHasRoom(49));
  EXPECT_FALSE(CheckGroupHasRoom(50));
  EXPECT_FALSE(CheckGroupHasRoom(51));
}

TEST(GroupInviteBatchTest, OneFailureDoesNotStopTheOthersAndIsNamed) {
  std::vector<std::string> tried;
  const GroupInviteBatchResult result = InviteEach({"ann", "bob", "cy"}, [&](const std::string& contact_id) -> Roe<void> {
    tried.push_back(contact_id);
    if (contact_id == "bob") {
      return Error("Contact has no Account ID");
    }
    return {};
  });
  EXPECT_EQ(tried, (std::vector<std::string>{"ann", "bob", "cy"}));
  EXPECT_EQ(result.sent, (std::vector<std::string>{"ann", "cy"}));
  ASSERT_EQ(result.failed.size(), 1u);
  EXPECT_EQ(result.failed[0].first, "bob");
  EXPECT_EQ(result.failed[0].second, "Contact has no Account ID");
}

TEST(GroupInviteBatchTest, AllSentLeavesNothingFailed) {
  const GroupInviteBatchResult result =
      InviteEach({"ann", "bob"}, [](const std::string&) -> Roe<void> { return {}; });
  EXPECT_EQ(result.sent.size(), 2u);
  EXPECT_TRUE(result.failed.empty());
}
