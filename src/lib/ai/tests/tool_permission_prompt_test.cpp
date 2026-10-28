// The permission card: plain-words descriptions of the calls (B114) and the rewrite of a card that has ended.
#include "lib/ai/ToolPermissionPrompt.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

class ToolPermissionPromptTest : public ::testing::TestWithParam<const char*> {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(std::string(PP_BROWSER_SOURCE_ROOT) + "/assets"));
    LocalizationService::Instance().SetPreferredLanguage(GetParam());
  }
  static bool IsEnglish() { return std::string(GetParam()) == "en"; }
};

INSTANTIATE_TEST_SUITE_P(Locales, ToolPermissionPromptTest, ::testing::Values("en", "zh-Hans"));

PlannedToolCall Call(const std::string& name, const std::string& key = {}, const std::string& value = {}) {
  PlannedToolCall call;
  call.name = name;
  if (!key.empty()) {
    call.arguments.set(key, value);
  }
  return call;
}

PlannedToolCall SwitchCall(const std::string& name, const bool enabled) {
  PlannedToolCall call;
  call.name = name;
  call.arguments.set("enabled", enabled);
  return call;
}

} // namespace

TEST_P(ToolPermissionPromptTest, AppearanceCardNamesTheValueNotTheTool) {
  const std::string text = DescribeToolCalls({Call("set_appearance", "appearance", "dark")});
  EXPECT_EQ(text, IsEnglish() ? "change the appearance to “Dark”" : "把外观改成「深色」");
  EXPECT_EQ(text.find("set_appearance"), std::string::npos);
  EXPECT_EQ(text.find("appearance="), std::string::npos);
}

TEST_P(ToolPermissionPromptTest, SwitchesSayOnOrOff) {
  EXPECT_EQ(DescribeToolCalls({SwitchCall("set_notifications", false)}),
            IsEnglish() ? "turn notifications off" : "关掉通知");
  EXPECT_EQ(DescribeToolCalls({SwitchCall("set_notifications", true)}),
            IsEnglish() ? "turn notifications on" : "打开通知");
  // No value in the call: the neutral wording, still no tool name.
  EXPECT_EQ(DescribeToolCalls({Call("set_notifications")}),
            IsEnglish() ? "change the notification setting" : "更改通知设置");
}

TEST_P(ToolPermissionPromptTest, ValuesUseTheOptionLabelsAndLongTextIsCut) {
  EXPECT_EQ(DescribeToolCalls({Call("set_call_invite_policy", "policy", "contacts_only")}),
            IsEnglish() ? "set who can call you to “Contacts only”" : "把谁可以给你打电话设为「仅联系人」");
  const std::string nickname = DescribeToolCalls({Call("update_profile_nickname", "nickname", std::string(80, 'x'))});
  EXPECT_NE(nickname.find(std::string(40, 'x') + "\xE2\x80\xA6"), std::string::npos);
  EXPECT_EQ(nickname.find(std::string(41, 'x')), std::string::npos);
}

TEST_P(ToolPermissionPromptTest, UnknownToolReadsGenericAndIsListedOnce) {
  const std::string generic = IsEnglish() ? "make a change in the app" : "在 App 里做一项修改";
  EXPECT_EQ(DescribeToolCalls({Call("some_mcp_tool", "path", "/etc/passwd")}), generic);
  EXPECT_EQ(DescribeToolCalls({Call("some_mcp_tool"), Call("other_mcp_tool")}), generic);
  const std::string two = DescribeToolCalls({Call("register_user"), Call("start_conversation")});
  EXPECT_NE(two.find(IsEnglish() ? ", " : "、"), std::string::npos);
}

TEST_P(ToolPermissionPromptTest, CardRewriteRemovesTheButtons) {
  const std::vector<PlannedToolCall> tools = {Call("set_appearance", "appearance", "dark")};
  const std::string card = BuildToolPermissionChoiceBlocks("apr-1", tools, true);
  ASSERT_NE(card.find("tool_permission"), std::string::npos);

  const std::string intro = Tr("chat.permission.intro");
  struct Case {
    ApprovalOutcome outcome;
    const char* key;
  };
  for (const Case& c : {Case{ApprovalOutcome::AllowOnce, "chat.permission.done.allow_once"},
                        Case{ApprovalOutcome::AllowAlways, "chat.permission.done.allow_always"},
                        Case{ApprovalOutcome::Deny, "chat.permission.done.deny"}}) {
    const auto rewritten = RewriteToolPermissionCardBlocks(card, c.outcome);
    ASSERT_TRUE(rewritten.has_value());
    EXPECT_EQ(rewritten->find("tool_permission"), std::string::npos) << "buttons must be gone";
    EXPECT_EQ(rewritten->find("choice"), std::string::npos);
    EXPECT_NE(rewritten->find(intro), std::string::npos) << "the explanation stays";
    EXPECT_NE(rewritten->find(DescribeToolCalls(tools)), std::string::npos) << "the answered card still names the action";
    EXPECT_NE(rewritten->find(Tr(c.key)), std::string::npos);
  }

  const auto stale = RewriteToolPermissionCardBlocks(card, ApprovalOutcome::Stale);
  ASSERT_TRUE(stale.has_value());
  EXPECT_EQ(*stale, BuildToolPermissionStaleBlocks(""));
  EXPECT_EQ(stale->find("tool_permission"), std::string::npos);
  EXPECT_NE(stale->find(Tr("chat.permission.stale")), std::string::npos);

  EXPECT_FALSE(RewriteToolPermissionCardBlocks("not json", ApprovalOutcome::Deny).has_value());
}

TEST_P(ToolPermissionPromptTest, NewKeysExist) {
  for (const char* key : {"chat.permission.busy_elsewhere", "chat.permission.done.allow_once",
                          "chat.permission.done.allow_always", "chat.permission.done.deny",
                          "chat.permission.tool.generic", "chat.permission.stale"}) {
    EXPECT_NE(Tr(key), key) << key << " missing in " << GetParam();
  }
}

} // namespace pbr
