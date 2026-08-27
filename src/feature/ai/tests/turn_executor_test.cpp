#include "feature/ai/TurnExecutor.h"
#include "base/ai/TurnPlan.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include <string>

namespace {

void FillEchoRegistry(pbr::ToolRegistry& registry) {
  pbr::Object query_prop;
  query_prop.set("type", "string");
  pbr::Object properties;
  properties.set("query", query_prop);
  pbr::Object parameters;
  parameters.set("type", "object");
  parameters.set("properties", properties);

  registry.Register(pbr::ToolDescriptor{
      .definition =
          pbr::ToolDefinition{
              .name = "echo_tool",
              .description = "echo",
              .parameters = std::move(parameters),
          },
      .execute = [](const pbr::Object& args) -> pbr::Roe<std::string> {
        return pbr::DumpJson(args);
      },
  });
}

} // namespace

TEST(TurnExecutorTest, ExecutesToolAndBuildsScratchMessages) {
  pbr::TurnPlan plan;
  pbr::Object args;
  args.set("query", "hello");
  plan.tools.push_back({.name = "echo_tool", .arguments = std::move(args)});

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
