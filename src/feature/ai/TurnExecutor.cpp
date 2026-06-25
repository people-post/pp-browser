#include "feature/ai/TurnExecutor.h"

#include "base/ai/ToolResultFormatter.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/PeopleDiscoveryBlocks.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

nlohmann::json ToolCallsToJson(const std::vector<PlannedToolCall>& planned, const std::string& id_prefix) {
  nlohmann::json out = nlohmann::json::array();
  for (size_t i = 0; i < planned.size(); ++i) {
    out.push_back({{"id", id_prefix + std::to_string(i + 1)},
                   {"type", "function"},
                   {"function", {{"name", planned[i].name}, {"arguments", planned[i].arguments.dump()}}}});
  }
  return out;
}

void ParsePeopleToolJson(const std::string& raw, std::vector<DirectoryHit>& hits, std::vector<Contact>& contacts) {
  const nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
  if (doc.is_discarded() || !doc.is_array()) {
    return;
  }
  for (const auto& item : doc) {
    if (!item.is_object()) {
      continue;
    }
    if (item.contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(item));
    } else if (item.contains("id") && item.contains("display_name")) {
      contacts.push_back(ContactFromJson(item));
    }
  }
}

bool IsPeopleDiscoveryTool(const std::string& name) {
  return name == "search_people" || name == "list_contacts";
}

} // namespace

TurnExecutionResult TurnExecutor::Execute(const TurnPlan& plan, ToolRegistry& tools,
                                          const ToolActivityCallback& on_activity) {
  TurnExecutionResult result;
  if (plan.tools.empty()) {
    return result;
  }

  std::vector<PlannedToolCall> executed_calls;
  std::vector<std::string> raw_results;
  executed_calls.reserve(plan.tools.size());
  raw_results.reserve(plan.tools.size());

  for (const PlannedToolCall& call : plan.tools) {
    if (on_activity) {
      on_activity(call.name, "running");
    }

    auto tool_result = tools.Execute(call.name, call.arguments);
    if (!tool_result) {
      if (on_activity) {
        on_activity(call.name, "error");
      }
      result.ok = false;
      result.error = tool_result.error().message;
      return result;
    }

    if (on_activity) {
      on_activity(call.name, "done");
    }

    executed_calls.push_back(call);
    raw_results.push_back(*tool_result);
    result.tools_executed.push_back(call.name);
  }

  ChatMessage assistant_message;
  assistant_message.role = "assistant";
  assistant_message.content = "";
  assistant_message.tool_calls = ToolCallsToJson(executed_calls, "planned_");
  result.scratch_append.push_back(std::move(assistant_message));

  for (size_t i = 0; i < executed_calls.size(); ++i) {
    ChatMessage tool_message;
    tool_message.role = "tool";
    tool_message.tool_call_id = "planned_" + std::to_string(i + 1);
    tool_message.content = FormatToolResultForLlm(executed_calls[i].name, raw_results[i]);
    result.scratch_append.push_back(std::move(tool_message));
  }

  if (plan.render_mode == RenderMode::PeopleList) {
    std::vector<DirectoryHit> hits;
    std::vector<Contact> contacts;
    for (const std::string& raw : raw_results) {
      ParsePeopleToolJson(raw, hits, contacts);
    }
    result.people_list_blocks = BuildPeopleDiscoveryBlocksJson(hits, contacts);
  }

  return result;
}

} // namespace pbr
