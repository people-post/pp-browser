// Group invite / owner-unreachable card buttons are labelled by payload type, in the user's language.
#include "gui/chat/ChatActionLabel.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/messaging/GroupMembershipCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

class ChatActionLabelTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(ChatActionLabelTest, GroupCardActionsAreLocalizedByPayloadType) {
  GroupInvitePayload invite;
  invite.invite_nonce = "n1";
  invite.inviter_identity = "inviter";
  const auto invite_actions = GroupMembershipCodec::BuildInviteChatActions(invite);
  ASSERT_EQ(invite_actions.size(), 3u);
  EXPECT_EQ(ChatActionLabel(invite_actions[0]), "接受");
  EXPECT_EQ(ChatActionLabel(invite_actions[1]), "拒绝");
  EXPECT_EQ(ChatActionLabel(invite_actions[2]), "屏蔽");

  const auto owner_actions = GroupMembershipCodec::BuildOwnerUnreachableChatActions("g1", "owner");
  ASSERT_EQ(owner_actions.size(), 3u);
  EXPECT_EQ(ChatActionLabel(owner_actions[0]), "新建群聊");
  EXPECT_EQ(ChatActionLabel(owner_actions[1]), "联系群主");
  EXPECT_EQ(ChatActionLabel(owner_actions[2]), "知道了");
}

TEST_F(ChatActionLabelTest, UnknownOrMissingTypeKeepsTheStoredLabel) {
  EXPECT_EQ(ChatActionLabel({.label = "Open", .message = "m", .payload = R"({"type":"something_else"})"}), "Open");
  EXPECT_EQ(ChatActionLabel({.label = "Open", .message = "m", .payload = "not json"}), "Open");
  EXPECT_EQ(ChatActionLabel({.label = "Open", .message = "m", .payload = std::nullopt}), "Open");
  EXPECT_FALSE(ChatActionLabelKey({.label = "x", .message = "m", .payload = R"({"a":1})"}));
}

TEST_F(ChatActionLabelTest, EnglishKeepsTheOriginalWords) {
  LocalizationService::Instance().SetPreferredLanguage("en");
  GroupInvitePayload invite;
  const auto actions = GroupMembershipCodec::BuildInviteChatActions(invite);
  for (const auto& action : actions) {
    EXPECT_EQ(ChatActionLabel(action), action.label);
  }
}

} // namespace
} // namespace pbr
