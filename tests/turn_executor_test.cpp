#include "agent/TurnExecutor.h"
#include "agent/TurnPlan.h"

#include <cassert>
#include <string>

namespace {

pbr::ToolRegistry MakeRegistry() {
  pbr::ToolRegistry registry;
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
  return registry;
}

} // namespace

int main() {
  pbr::TurnPlan plan;
  plan.tools.push_back({.name = "echo_tool", .arguments = {{"query", "hello"}}});

  pbr::ToolRegistry registry = MakeRegistry();
  const pbr::TurnExecutionResult result = pbr::TurnExecutor::Execute(plan, registry);
  assert(result.ok);
  assert(result.tools_executed.size() == 1);
  assert(result.scratch_append.size() == 2);
  assert(result.scratch_append[0].role == "assistant");
  assert(result.scratch_append[1].role == "tool");
  assert(!result.people_list_blocks);

  return 0;
}
