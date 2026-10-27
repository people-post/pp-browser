#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/MessagingTools.h"

#include <gtest/gtest.h>

TEST(MessagingToolsTest, FindingAFriendOffersSearchAndLeavesAdditionToTheSelectedRow) {
  pbr::ConversationsHub hub;
  pbr::ConversationsFacade facade(hub);
  pbr::ToolRegistry registry;
  pbr::RegisterMessagingTools(registry, facade);

  bool has_search = false;
  for (const auto& tool : registry.Tools()) {
    EXPECT_NE(tool.definition.name, "add_contact");
    if (tool.definition.name == "search_people") {
      has_search = true;
      EXPECT_EQ(tool.meta.risk, "read");
      EXPECT_FALSE(tool.meta.mutating);
    }
  }
  EXPECT_TRUE(has_search);
  // A stale model/tool call cannot bypass choosing a real result, even with a saved permission grant.
  EXPECT_FALSE(registry.Execute("add_contact", {}));
}
