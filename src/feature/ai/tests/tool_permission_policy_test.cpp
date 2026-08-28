#include "feature/ai/ToolPermissionPolicy.h"
#include "feature/ai/ToolPermissionPrompt.h"
#include "feature/ai/TurnExecutor.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>

namespace {

pbr::Object EmptyObjectSchema() {
  pbr::Object schema;
  schema.set("type", "object");
  schema.set("properties", pbr::Object{});
  return schema;
}

} // namespace

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
      pbr::BuildToolPermissionChoiceBlocks("apr-1", {{.name = "add_contact", .arguments = {}}});
  EXPECT_NE(blocks.find("tool_permission"), std::string::npos);
  EXPECT_NE(blocks.find("apr-1"), std::string::npos);
  EXPECT_NE(blocks.find("allow_once"), std::string::npos);
  EXPECT_NE(blocks.find("allow_always"), std::string::npos);
  EXPECT_NE(blocks.find("deny"), std::string::npos);
}

TEST(TurnExecutorPermissionTest, StopsBeforeMutatingAskTool) {
  pbr::ToolRegistry registry;
  registry.Register(pbr::MakeTool(
      {.name = "list_contacts", .description = "list", .parameters = EmptyObjectSchema()},
      {.domain = "people", .risk = "read", .mutating = false},
      [](const pbr::Object&) -> pbr::Roe<std::string> { return std::string("[]"); }));
  registry.Register(pbr::MakeTool(
      {.name = "add_contact", .description = "add", .parameters = EmptyObjectSchema()},
      {.domain = "people", .risk = "write", .mutating = true},
      [](const pbr::Object&) -> pbr::Roe<std::string> {
        ADD_FAILURE() << "mutating tool should not execute before permission";
        return std::string("{}");
      }));

  pbr::TurnPlan plan;
  plan.tools.push_back({.name = "list_contacts", .arguments = {}});
  plan.tools.push_back({.name = "add_contact", .arguments = {}});

  pbr::TurnExecutionOptions options;
  const pbr::TurnExecutionResult result = pbr::TurnExecutor::Execute(plan, registry, {}, options);
  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.needs_permission);
  ASSERT_EQ(result.tools_executed.size(), 1u);
  EXPECT_EQ(result.tools_executed[0], "list_contacts");
  ASSERT_EQ(result.offered_tools.size(), 1u);
  EXPECT_EQ(result.offered_tools[0].name, "add_contact");
  EXPECT_EQ(result.next_tool_index, 1u);
}
