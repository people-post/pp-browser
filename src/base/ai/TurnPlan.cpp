#include "base/ai/TurnPlan.h"

#include <algorithm>
#include <cctype>

namespace pbr {

namespace {

constexpr int kMaxPlannedTools = 4;

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string JsonStringOrDefault(const nlohmann::json& json, const char* key,
                                const std::string& default_value = {}) {
  if (!json.contains(key)) {
    return default_value;
  }
  const auto& value = json[key];
  if (value.is_string()) {
    return value.get<std::string>();
  }
  return default_value;
}

std::optional<nlohmann::json> ExtractJsonObject(const std::string& text) {
  const std::string fence = "```json";
  const size_t start = text.find(fence);
  if (start != std::string::npos) {
    size_t content_start = start + fence.size();
    while (content_start < text.size() && (text[content_start] == '\n' || text[content_start] == '\r')) {
      ++content_start;
    }
    const size_t end = text.find("```", content_start);
    if (end != std::string::npos) {
      const std::string fenced = Trim(text.substr(content_start, end - content_start));
      const nlohmann::json doc = nlohmann::json::parse(fenced, nullptr, false);
      if (!doc.is_discarded() && doc.is_object()) {
        return doc;
      }
    }
  }

  const std::string trimmed = Trim(text);
  const nlohmann::json bare = nlohmann::json::parse(trimmed, nullptr, false);
  if (!bare.is_discarded() && bare.is_object()) {
    return bare;
  }
  return std::nullopt;
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

Roe<TurnPlan> ParseTurnPlanJson(const nlohmann::json& doc, const TurnPlanSource source) {
  if (!doc.is_object()) {
    return Error("Turn plan must be a JSON object");
  }

  TurnPlan plan;
  plan.source = source;
  plan.response_goal = ParseResponseGoal(JsonStringOrDefault(doc, "response_goal", "general"));
  plan.render_mode = ParseRenderMode(JsonStringOrDefault(doc, "render_mode", "blocks"));
  plan.synthesis_hints = JsonStringOrDefault(doc, "synthesis_hints");
  plan.user_request = JsonStringOrDefault(doc, "user_request");

  if (doc.contains("tools") && doc["tools"].is_array()) {
    for (const auto& item : doc["tools"]) {
      if (!item.is_object()) {
        return Error("Each planned tool must be an object");
      }
      PlannedToolCall call;
      call.name = JsonStringOrDefault(item, "name");
      if (call.name.empty()) {
        return Error("Planned tool missing name");
      }
      if (item.contains("arguments") && item["arguments"].is_object()) {
        call.arguments = item["arguments"];
      } else if (item.contains("args") && item["args"].is_object()) {
        call.arguments = item["args"];
      } else {
        call.arguments = nlohmann::json::object();
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

nlohmann::json TurnPlanToJson(const TurnPlan& plan) {
  nlohmann::json tools = nlohmann::json::array();
  for (const PlannedToolCall& call : plan.tools) {
    tools.push_back({{"name", call.name}, {"arguments", call.arguments}});
  }

  return {{"source", TurnPlanSourceName(plan.source)},
          {"response_goal", ResponseGoalName(plan.response_goal)},
          {"tools", std::move(tools)},
          {"render_mode", RenderModeName(plan.render_mode)},
          {"synthesis_hints", plan.synthesis_hints},
          {"user_request", plan.user_request}};
}

} // namespace pbr
