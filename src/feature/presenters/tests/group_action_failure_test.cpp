#include "feature/presenters/ChatPresenter.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupRosterSync.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The peer sheet shows these lines instead of the engine's English error text.
class GroupActionFailureTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
};

TEST_F(GroupActionFailureTest, EngineErrorsMapToLocalizedLines) {
  EXPECT_EQ(GroupActionFailureText(Error("Only the group owner may rename the group for everyone")),
            "Only the group owner can do this.");
  EXPECT_EQ(GroupActionFailureText(Error("Only the group owner may remove members")),
            "Only the group owner can do this.");
  EXPECT_EQ(GroupActionFailureText(Error("Group title required")), "Enter a name");
  EXPECT_EQ(GroupActionFailureText(Error(group_roster_sync::kGroupDissolvedError)), "This group was dissolved.");
}

TEST_F(GroupActionFailureTest, AnUnknownErrorNeverShowsItsRawText) {
  const std::string text = GroupActionFailureText(Error("sqlite3: disk I/O error"));
  EXPECT_EQ(text, "Couldn't update the group. Please try again.");
  EXPECT_EQ(text.find("sqlite"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(GroupActionFailureText(Error("sqlite3: disk I/O error")), "暂时无法更新群。请稍后再试。");
  LocalizationService::Instance().SetPreferredLanguage("en");
}

} // namespace
} // namespace pbr
