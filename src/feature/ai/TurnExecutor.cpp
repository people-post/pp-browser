#include "feature/ai/TurnExecutor.h"

#include "base/ai/ToolResultFormatter.h"
#include "feature/ai/PeopleDiscoveryContactAdapt.h"
#include "base/people/ContactJson.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Value ToolCallsToJson(const std::vector<PlannedToolCall>& planned, const std::string& id_prefix,
                      const size_t id_offset = 0) {
  std::vector<Value> out;
  out.reserve(planned.size());
  for (size_t i = 0; i < planned.size(); ++i) {
    Object function;
    function.set("name", planned[i].name);
    function.set("arguments", DumpJson(planned[i].arguments));

    Object entry;
    entry.set("id", id_prefix + std::to_string(id_offset + i + 1));
    entry.set("type", "function");
    entry.set("function", function);
    out.push_back(ObjectValue(std::move(entry)));
  }
  return ArrayValue(std::move(out));
}

void ParsePeopleToolJson(const std::string& raw, std::vector<DirectoryHit>& hits, std::vector<Contact>& contacts) {
  auto parsed = ParseValue(raw);
  if (!parsed) {
    return;
  }
  const Array* doc = asArray(*parsed);
  if (!doc) {
    return;
  }
  for (const Value& item_value : doc->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    if (item->contains("hit_id")) {
      hits.push_back(DirectoryHitFromJson(*item));
    } else if (item->contains("id") && item->contains("display_name")) {
      contacts.push_back(ContactFromJson(*item));
    }
  }
}

const ToolDescriptor* FindTool(const ToolRegistry& tools, const std::string& name) {
  for (const ToolDescriptor& tool : tools.Tools()) {
    if (tool.definition.name == name) {
      return &tool;
    }
  }
  return nullptr;
}

} // namespace

TurnExecutionResult TurnExecutor::Execute(const TurnPlan& plan, ToolRegistry& tools,
                                          const ToolActivityCallback& on_activity,
                                          const TurnExecutionOptions& options) {
  TurnExecutionResult result;
  if (plan.tools.empty() || options.start_index >= plan.tools.size()) {
    return result;
  }

  std::vector<PlannedToolCall> executed_calls;
  std::vector<std::string> raw_results;
  executed_calls.reserve(plan.tools.size());
  raw_results.reserve(plan.tools.size());

  for (size_t i = options.start_index; i < plan.tools.size(); ++i) {
    const PlannedToolCall& call = plan.tools[i];
    const ToolDescriptor* descriptor = FindTool(tools, call.name);
    ToolMeta meta;
    if (descriptor) {
      meta = descriptor->meta;
    } else {
      // Unknown tools are validated earlier; treat as read if somehow present.
      meta.risk = "read";
    }

    const ToolPermissionEval eval =
        ToolPermissionPolicy::Evaluate(call.name, meta, options.permissions, options.session_grants);

    if (eval.verdict == ToolPermissionVerdict::Ask) {
      if (options.deny_on_ask) {
        if (on_activity) {
          on_activity(call.name, "error");
        }
        result.ok = false;
        result.error = "Permission required for tool '" + call.name +
                       "' — confirm in chat before the assistant can change your data.";
        return result;
      }

      result.needs_permission = true;
      result.next_tool_index = i;
      for (size_t j = i; j < plan.tools.size(); ++j) {
        const ToolDescriptor* offered_desc = FindTool(tools, plan.tools[j].name);
        ToolMeta offered_meta;
        if (offered_desc) {
          offered_meta = offered_desc->meta;
        }
        const ToolPermissionEval offered_eval = ToolPermissionPolicy::Evaluate(
            plan.tools[j].name, offered_meta, options.permissions, options.session_grants);
        if (offered_eval.verdict == ToolPermissionVerdict::Ask) {
          result.offered_tools.push_back(plan.tools[j]);
        }
      }
      break;
    }

    if (eval.verdict == ToolPermissionVerdict::Deny) {
      if (on_activity) {
        on_activity(call.name, "error");
      }
      result.ok = false;
      result.error = "Tool '" + call.name + "' is blocked by your tool permission settings.";
      return result;
    }

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

  if (executed_calls.empty()) {
    return result;
  }

  ChatMessage assistant_message;
  assistant_message.role = "assistant";
  assistant_message.content = "";
  assistant_message.tool_calls = ToolCallsToJson(executed_calls, "planned_", options.start_index);
  result.scratch_append.push_back(std::move(assistant_message));

  for (size_t i = 0; i < executed_calls.size(); ++i) {
    ChatMessage tool_message;
    tool_message.role = "tool";
    tool_message.tool_call_id = "planned_" + std::to_string(options.start_index + i + 1);
    tool_message.content = FormatToolResultForLlm(executed_calls[i].name, raw_results[i]);
    result.scratch_append.push_back(std::move(tool_message));
  }

  if (plan.render_mode == RenderMode::PeopleList && !result.needs_permission) {
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
