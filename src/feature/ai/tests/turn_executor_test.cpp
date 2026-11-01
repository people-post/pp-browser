#include "feature/ai/TurnExecutor.h"
#include "lib/ai/TurnPlan.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>
#include <string>
#include "common/PbrCompat.h"

namespace {

void FillEchoRegistry(pbr::ToolRegistry& registry) {
  pbr::Object query_prop;
  query_prop.set("type", "string");
  pbr::Object properties;
  properties.set("query", query_prop);
  pbr::Object parameters;
  parameters.set("type", "object");
  parameters.set("properties", properties);

  registry.Register(pbr::MakeTool(
      pbr::ToolDefinition{"echo_tool", "echo", std::move(parameters)},
      pbr::ToolMeta{},
      [](const pbr::Object& args) -> pbr::Roe<std::string> {
        return pbr::DumpJson(args);
      }));
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

// The tools' contact JSON is model-facing and carries no remark; the people-list row still shows it (remark first,
// nickname in small print), taken from the local contacts on the device.
TEST(TurnExecutorTest, ContactRowsShowTheRemarkFromTheLocalContacts) {
  pbr::ToolRegistry registry;
  registry.Register(pbr::MakeTool(
      pbr::ToolDefinition{"list_contacts", "contacts", pbr::Object{}}, pbr::ToolMeta{.risk = "read"},
      [](const pbr::Object&) -> pbr::Roe<std::string> {
        return std::string(R"([{"id":"c1","local":{"trust":"unknown"},"remote":{"nickname":"Wang"}}])");
      }));
  pbr::TurnPlan plan;
  plan.render_mode = pbr::RenderMode::PeopleList;
  plan.response_goal = pbr::ResponseGoal::PeopleDiscovery;
  plan.tools.push_back({.name = "list_contacts", .arguments = pbr::Object{}});

  pbr::TurnExecutionOptions options;
  options.local_contacts = [] {
    return std::vector<pbr::LocalPlanContact>{{.id = "c1", .remark = "欠钱的老王", .nickname = "Wang"}};
  };
  const pbr::TurnExecutionResult result = pbr::TurnExecutor::Execute(plan, registry, {}, options);
  ASSERT_TRUE(result.people_list_blocks.has_value());
  const std::string& blocks = *result.people_list_blocks;
  EXPECT_NE(blocks.find(R"("title":"欠钱的老王")"), std::string::npos) << blocks;
  EXPECT_NE(blocks.find("~Wang"), std::string::npos) << blocks;
  // The model-facing scratch (what a model would read) has no remark.
  for (const pbr::ChatMessage& message : result.scratch_append) {
    EXPECT_EQ(message.content.find("欠钱的老王"), std::string::npos) << message.content;
  }

  // Without the port the row is the nickname, as before.
  const pbr::TurnExecutionResult plain = pbr::TurnExecutor::Execute(plan, registry);
  ASSERT_TRUE(plain.people_list_blocks.has_value());
  EXPECT_EQ(plain.people_list_blocks->find("欠钱的老王"), std::string::npos);
}
