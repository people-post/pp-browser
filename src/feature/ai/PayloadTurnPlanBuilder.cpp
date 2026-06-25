#include "feature/ai/PayloadTurnPlanBuilder.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace pbr {

namespace {

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
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

bool IsMcpArticleFeedToolName(const std::string& tool_name) {
  if (tool_name == "blog_articles") {
    return true;
  }
  const std::string lower = Lower(tool_name);
  return lower.find("article") != std::string::npos || lower.find("feed") != std::string::npos;
}

nlohmann::json ToolArgumentsFromPayload(const nlohmann::json& doc) {
  nlohmann::json args = nlohmann::json::object();
  for (auto it = doc.begin(); it != doc.end(); ++it) {
    if (it.key() == "tool" || it.key() == "type" || it.key() == "message" || it.key() == "label") {
      continue;
    }
    args[it.key()] = it.value();
  }
  return args;
}

} // namespace

std::optional<TurnPlan> TryBuildPlanFromPayload(const std::string& user_text, const std::string& payload) {
  if (payload.empty()) {
    return std::nullopt;
  }

  const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return std::nullopt;
  }

  TurnPlan plan;
  plan.source = TurnPlanSource::Payload;
  plan.user_request = Trim(user_text);
  if (plan.user_request.empty()) {
    plan.user_request = payload;
  }

  const std::string type = JsonStringOrDefault(doc, "type");
  if (type == "article") {
    plan.response_goal = ResponseGoal::Summarize;
    plan.render_mode = RenderMode::Blocks;
    plan.synthesis_hints =
        "Summarize the referenced article concisely in a heading plus paragraph or card block.";
    return plan;
  }

  if (type == "form_submission") {
    plan.response_goal = ResponseGoal::General;
    plan.render_mode = RenderMode::Blocks;
    plan.synthesis_hints =
        "Acknowledge the submitted form values and respond helpfully based on the user's display message.";
    return plan;
  }

  const std::string tool = JsonStringOrDefault(doc, "tool");
  if (!tool.empty()) {
    PlannedToolCall call;
    call.name = tool;
    call.arguments = ToolArgumentsFromPayload(doc);

    if (tool == "blog_articles" || IsMcpArticleFeedToolName(tool)) {
      plan.response_goal = ResponseGoal::DisplayFeed;
      plan.render_mode = RenderMode::Blocks;
      plan.synthesis_hints =
          "Map MCP feed tool results into a long_list block with a short intro paragraph.";
      plan.tools.push_back(std::move(call));
      return plan;
    }

    if (tool == "search_people" || tool == "list_contacts") {
      plan.response_goal = ResponseGoal::PeopleDiscovery;
      plan.render_mode = RenderMode::PeopleList;
      plan.synthesis_hints = "Render people discovery results as a long_list.";
      plan.tools.push_back(std::move(call));
      return plan;
    }

    plan.response_goal = ResponseGoal::General;
    plan.render_mode = RenderMode::Blocks;
    plan.synthesis_hints = "Use the tool results to address the user's request.";
    plan.tools.push_back(std::move(call));
    return plan;
  }

  return std::nullopt;
}

} // namespace pbr
