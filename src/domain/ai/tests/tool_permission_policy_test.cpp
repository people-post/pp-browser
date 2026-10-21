#include "domain/ai/ToolPermissionPolicy.h"
#include "domain/ai/ToolPermissionPrompt.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

TEST(ToolPermissionPolicyTest, DefaultsAskForWriteAllowForRead) {
  pbr::ToolPermissionsPrefs prefs;
  pbr::ToolMeta read_meta{.domain = "knowledge", .risk = "read", .mutating = false};
  pbr::ToolMeta write_meta{.domain = "people", .risk = "write", .mutating = true};

  EXPECT_EQ(pbr::ToolPermissionPolicy::Evaluate("web_search", read_meta, prefs).verdict,
            pbr::ToolPermissionVerdict::Allow);
  EXPECT_EQ(pbr::ToolPermissionPolicy::Evaluate("add_contact", write_meta, prefs).verdict,
            pbr::ToolPermissionVerdict::Ask);
}

TEST(ToolPermissionPolicyTest, ByToolAndSessionGrant) {
  pbr::ToolPermissionsPrefs prefs;
  pbr::ToolMeta write_meta{.risk = "write", .mutating = true};
  pbr::ToolPermissionPolicy::SetToolDecision(prefs, "add_contact", "allow");
  EXPECT_EQ(pbr::ToolPermissionPolicy::Evaluate("add_contact", write_meta, prefs).verdict,
            pbr::ToolPermissionVerdict::Allow);

  pbr::ToolPermissionPolicy::SetToolDecision(prefs, "add_contact", "deny");
  EXPECT_EQ(pbr::ToolPermissionPolicy::Evaluate("add_contact", write_meta, prefs).verdict,
            pbr::ToolPermissionVerdict::Deny);

  std::unordered_set<std::string> grants{"add_contact"};
  EXPECT_EQ(pbr::ToolPermissionPolicy::Evaluate("add_contact", write_meta, prefs, grants).verdict,
            pbr::ToolPermissionVerdict::Allow);
}

TEST(ToolPermissionPromptTest, BuildsChoiceWithApprovalId) {
  const std::string blocks =
      pbr::BuildToolPermissionChoiceBlocks("apr-1", {{.name = "add_contact", .arguments = {}}}, true);
  EXPECT_NE(blocks.find("tool_permission"), std::string::npos);
  EXPECT_NE(blocks.find("apr-1"), std::string::npos);
  EXPECT_NE(blocks.find("allow_once"), std::string::npos);
  EXPECT_NE(blocks.find("allow_always"), std::string::npos);
  EXPECT_NE(blocks.find("deny"), std::string::npos);
}

TEST(ToolPermissionPolicyTest, AllowAlwaysIsOnlyForOrdinaryWriteTools) {
  EXPECT_TRUE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "people", .risk = "write", .mutating = true}));
  EXPECT_TRUE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "settings", .risk = "write", .mutating = true}));
  // Security and identity settings, and anything destructive, are confirmed every time.
  EXPECT_FALSE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "security", .risk = "write", .mutating = true}));
  EXPECT_FALSE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "identity", .risk = "write", .mutating = true}));
  EXPECT_FALSE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "people", .risk = "destructive", .mutating = true}));
  EXPECT_FALSE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({.domain = "people", .risk = "read", .mutating = false}));
  // An unknown tool (no descriptor) has an empty meta: never remembered.
  EXPECT_FALSE(pbr::ToolPermissionPolicy::AllowAlwaysEligible({}));
}

TEST(ToolPermissionPromptTest, AlwaysOptionIsOfferedOnlyWhenAllowed) {
  const std::vector<pbr::PlannedToolCall> tools = {{.name = "add_contact", .arguments = {}}};
  const std::string with = pbr::BuildToolPermissionChoiceBlocks("apr-1", tools, /*offer_allow_always=*/true);
  EXPECT_NE(with.find("allow_always"), std::string::npos);
  const std::string without = pbr::BuildToolPermissionChoiceBlocks("apr-1", tools, /*offer_allow_always=*/false);
  EXPECT_EQ(without.find("allow_always"), std::string::npos);
  EXPECT_NE(without.find("allow_once"), std::string::npos);
  EXPECT_NE(without.find("\"deny\""), std::string::npos);
}

TEST(ToolPermissionPromptTest, PromptShowsTheArgumentsThatWillBeUsed) {
  pbr::Object args;
  args.set("policy", "everyone");
  args.set("count", static_cast<int64_t>(3));
  args.set("notify", true);
  pbr::Object nested;
  nested.set("account_id", "account:abc");
  args.set("directory_hit", nested);
  args.set("note", std::string(80, 'x'));
  EXPECT_EQ(pbr::DescribeToolCalls({{.name = "set_call_invite_policy", .arguments = args}}),
            "set_call_invite_policy(policy=everyone, count=3, notify=true, directory_hit={\xE2\x80\xA6}, note=" +
                std::string(40, 'x') + "\xE2\x80\xA6)");
  // A tool without arguments is shown by name alone, as before; several calls are comma-separated.
  EXPECT_EQ(pbr::DescribeToolCalls({{.name = "add_contact", .arguments = {}}, {.name = "register_user", .arguments = {}}}),
            "add_contact, register_user");
  // The summary is what the prompt carries (the locale string is looked up at runtime).
  pbr::Object one;
  one.set("policy", "nobody");
  const std::string blocks = pbr::BuildToolPermissionChoiceBlocks(
      "apr-2", {{.name = "set_group_invite_policy", .arguments = one}}, false);
  EXPECT_NE(blocks.find("apr-2"), std::string::npos);
}
