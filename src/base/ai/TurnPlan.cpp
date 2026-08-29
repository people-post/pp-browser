#include "base/ai/TurnPlan.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <algorithm>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kMaxPlannedTools = 4;

std::string JsonStringOrDefault(const Object& json, const char* key, const std::string& default_value = {}) {
  return json.getString(key).value_or(default_value);
}

std::optional<Object> ExtractJsonObject(const std::string& text) {
  const std::string fence = "```json";
  const size_t start = text.find(fence);
  if (start != std::string::npos) {
    size_t content_start = start + fence.size();
    while (content_start < text.size() && (text[content_start] == '\n' || text[content_start] == '\r')) {
      ++content_start;
    }
    const size_t end = text.find("```", content_start);
    if (end != std::string::npos) {
      const std::string fenced = util::Trim(text.substr(content_start, end - content_start));
      if (auto doc = TryParseObject(fenced)) {
        return doc;
      }
    }
  }

  const std::string trimmed = util::Trim(text);
  return TryParseObject(trimmed);
}

} // namespace

const char* TurnPlanSourceName(const TurnPlanSource source) {
  switch (source) {
  case TurnPlanSource::Payload:
    return "payload";
  case TurnPlanSource::Planner:
    return "planner";
  }
  return "planner";
}

const char* ResponseGoalName(const ResponseGoal goal) {
  switch (goal) {
  case ResponseGoal::DisplayFeed:
    return "display_feed";
  case ResponseGoal::Summarize:
    return "summarize";
  case ResponseGoal::AnswerQuestion:
    return "answer_question";
  case ResponseGoal::Headlines:
    return "headlines";
  case ResponseGoal::PeopleDiscovery:
    return "people_discovery";
  case ResponseGoal::General:
    return "general";
  }
  return "general";
}

const char* RenderModeName(const RenderMode mode) {
  switch (mode) {
  case RenderMode::Blocks:
    return "blocks";
  case RenderMode::PeopleList:
    return "people_list";
  }
  return "blocks";
}

ResponseGoal ParseResponseGoal(const std::string& name) {
  if (name == "display_feed") {
    return ResponseGoal::DisplayFeed;
  }
  if (name == "summarize") {
    return ResponseGoal::Summarize;
  }
  if (name == "answer_question") {
    return ResponseGoal::AnswerQuestion;
  }
  if (name == "headlines") {
    return ResponseGoal::Headlines;
  }
  if (name == "people_discovery") {
    return ResponseGoal::PeopleDiscovery;
  }
  return ResponseGoal::General;
}

RenderMode ParseRenderMode(const std::string& name) {
  if (name == "people_list") {
    return RenderMode::PeopleList;
  }
  return RenderMode::Blocks;
}

Roe<TurnPlan> ParseTurnPlanJson(const Object& doc, const TurnPlanSource source) {
  TurnPlan plan;
  plan.source = source;
  plan.response_goal = ParseResponseGoal(JsonStringOrDefault(doc, "response_goal", "general"));
  plan.render_mode = ParseRenderMode(JsonStringOrDefault(doc, "render_mode", "blocks"));
  plan.synthesis_hints = JsonStringOrDefault(doc, "synthesis_hints");
  plan.user_request = JsonStringOrDefault(doc, "user_request");

  if (const Array* tools = doc.getArray("tools")) {
    for (const Value& item_value : tools->elements) {
      const Object* item = asObject(item_value);
      if (!item) {
        return Error("Each planned tool must be an object");
      }
      PlannedToolCall call;
      call.name = JsonStringOrDefault(*item, "name");
      if (call.name.empty()) {
        return Error("Planned tool missing name");
      }
      if (const Object* arguments = item->getObject("arguments")) {
        call.arguments = *arguments;
      } else if (const Object* args = item->getObject("args")) {
        call.arguments = *args;
      }
      plan.tools.push_back(std::move(call));
    }
  }

  return plan;
}

Roe<TurnPlan> ParseTurnPlanFromLlmOutput(const std::string& llm_output, const TurnPlanSource source) {
  const auto doc = ExtractJsonObject(llm_output);
  if (!doc) {
    return Error("Turn planner output missing valid JSON object");
  }
  return ParseTurnPlanJson(*doc, source);
}

Roe<TurnPlan> ValidateTurnPlan(TurnPlan plan, const std::vector<std::string>& allowed_tools) {
  if (plan.tools.size() > static_cast<size_t>(kMaxPlannedTools)) {
    return Error("Turn plan exceeds maximum planned tools");
  }

  for (const PlannedToolCall& call : plan.tools) {
    const auto it = std::find(allowed_tools.begin(), allowed_tools.end(), call.name);
    if (it == allowed_tools.end()) {
      return Error("Unknown planned tool: " + call.name);
    }
  }

  if (plan.render_mode == RenderMode::PeopleList && plan.response_goal != ResponseGoal::PeopleDiscovery) {
    plan.response_goal = ResponseGoal::PeopleDiscovery;
  }

  return plan;
}

Object TurnPlanToJson(const TurnPlan& plan) {
  std::vector<Value> tools;
  tools.reserve(plan.tools.size());
  for (const PlannedToolCall& call : plan.tools) {
    Object entry;
    entry.set("name", call.name);
    entry.set("arguments", call.arguments);
    tools.push_back(ObjectValue(std::move(entry)));
  }

  Object out;
  out.set("source", TurnPlanSourceName(plan.source));
  out.set("response_goal", ResponseGoalName(plan.response_goal));
  out.set("tools", ArrayValue(std::move(tools)));
  out.set("render_mode", RenderModeName(plan.render_mode));
  out.set("synthesis_hints", plan.synthesis_hints);
  out.set("user_request", plan.user_request);
  return out;
}

} // namespace pbr
