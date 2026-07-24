#include "feature/ai/TurnExecutor.h"
#include "base/ai/TurnPlan.h"

#include <gtest/gtest.h>
#include <string>

namespace {

void FillEchoRegistry(pbr::ToolRegistry& registry) {
  registry.Register(pbr::ToolDescriptor{
      .definition =
          pbr::ToolDefinition{
              .name = "echo_tool",
              .description = "echo",
              .parameters = {{"type", "object"}, {"properties", {{"query", {{"type", "string"}}}}}},
          },
      .execute = [](const nlohmann::json& args) -> pbr::Roe<std::string> {
        return args.dump();
      },
  });
}

} // namespace

TEST(TurnExecutorTest, ExecutesToolAndBuildsScratchMessages) {
  pbr::TurnPlan plan;
  plan.tools.push_back({.name = "echo_tool", .arguments = {{"query", "hello"}}});

  pbr::ToolRegistry registry;
  FillEchoRegistry(registry);
  const pbr::TurnExecutionResult result = pbr::TurnExecutor::Execute(plan, registry);
  EXPECT_TRUE(result.ok);
  ASSERT_EQ(result.tools_executed.size(), 1u);
  ASSERT_EQ(result.scratch_append.size(), 2u);
  EXPECT_EQ(result.scratch_append[0].role, "assistant");
  EXPECT_EQ(result.scratch_append[1].role, "tool");
  EXPECT_FALSE(result.people_list_blocks.has_value());
}
