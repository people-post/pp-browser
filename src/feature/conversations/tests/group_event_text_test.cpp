#include "feature/conversations/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The words of a group's membership lines are one function for the transcript and the chat list's preview.

const auto kNames = [](const std::string& account) { return account == "acct-ann" ? std::string("Ann") : account; };

ThreadMessage Event(const GroupMembershipControlType type, const Roe<std::string>& detail, const std::string& text,
                    const std::string& sender) {
  EXPECT_TRUE(detail);
  auto message = GroupMembershipCodec::BuildSystemMessage("t", type, text, *detail, sender);
  EXPECT_TRUE(message);
  return *message;
}

class GroupEventTextTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(GroupEventTextTest, TheChatListPreviewAndTheTranscriptUseTheSameChineseWords) {
  const auto text = GroupEventText(Event(GroupMembershipControlType::MemberJoined,
                                         GroupMembershipCodec::EncodeMemberJoined("g", "acct-ann", MemberRole::Member, 2),
                                         "Member joined the group", "acct-ann"),
                                   kNames, "acct-me");
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "Ann \xE5\x8A\xA0\xE5\x85\xA5\xE4\xBA\x86\xE7\xBE\xA4\xE8\x81\x8A");
}

TEST_F(GroupEventTextTest, TheStoredEnglishTextsAreRecognisedAsMembershipPreviews) {
  for (const char* stored : {"Member joined the group", "You joined the group", "Owner created the group",
                             "Member removed", "Member left", "Group ownership transferred",
                             "Ownership transferred; previous owner left", "Group renamed to Sunday hike",
                             "The group is full: you were not added", "Group forked", "Forked from group",
                             "Invitation declined"}) {
    EXPECT_TRUE(IsStoredGroupEventText(stored)) << stored;
  }
  EXPECT_FALSE(IsStoredGroupEventText("see you at 7"));
  EXPECT_FALSE(IsStoredGroupEventText("Member joined the group chat later"));
}

TEST_F(GroupEventTextTest, ForkedLinesTellTheTwoSidesApart) {
  const auto source = GroupEventText(Event(GroupMembershipControlType::GroupForked,
                                           Roe<std::string>(std::string("{}")), "Group forked", "acct-ann"),
                                     kNames, "acct-me");
  const auto fresh = GroupEventText(Event(GroupMembershipControlType::GroupForked,
                                          Roe<std::string>(std::string("{}")), "Forked from group", "acct-ann"),
                                    kNames, "acct-me");
  ASSERT_TRUE(source && fresh);
  EXPECT_EQ(*source, "Ann \xE4\xBB\x8E\xE6\x9C\xAC\xE7\xBE\xA4\xE5\x88\x9B\xE5\xBB\xBA\xE4\xBA\x86\xE6\x96\xB0\xE7\xBE\xA4\xE8\x81\x8A");
  EXPECT_NE(*source, *fresh);
}

} // namespace
} // namespace pbr
