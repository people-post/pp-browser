#include "lib/ai/ToolPermissionPolicy.h"
#include "feature/ai/TurnExecutor.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include "common/PbrCompat.h"

namespace {

pbr::Object EmptyObjectSchema() {
  pbr::Object schema;
  schema.set("type", "object");
  schema.set("properties", pbr::Object{});
  return schema;
}

} // namespace

TEST(TurnExecutorPermissionTest, StopsBeforeMutatingAskTool) {
  pbr::ToolRegistry registry;
  registry.Register(pbr::MakeTool(
      pbr::ToolDefinition{"list_contacts", "list", EmptyObjectSchema()},
      pbr::ToolMeta{.domain = "people", .risk = "read", .mutating = false},
      [](const pbr::Object&) -> pbr::Roe<std::string> { return std::string("[]"); }));
  registry.Register(pbr::MakeTool(
      pbr::ToolDefinition{"add_contact", "add", EmptyObjectSchema()},
      pbr::ToolMeta{.domain = "people", .risk = "write", .mutating = true},
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

// PB-015: the prompt names what will run with what, and "Always" is not offered for every write tool.
