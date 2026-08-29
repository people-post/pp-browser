#include "feature/ai/PayloadTurnPlanBuilder.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string JsonStringOrDefault(const Object& json, const char* key, const std::string& default_value = {}) {
  return json.getString(key).value_or(default_value);
}

bool IsMcpArticleFeedToolName(const std::string& tool_name) {
  if (tool_name == "blog_articles") {
    return true;
  }
  const std::string lower = util::ToLowerAscii(tool_name);
  return lower.find("article") != std::string::npos || lower.find("feed") != std::string::npos;
}

Object ToolArgumentsFromPayload(const Object& doc) {
  Object args;
  for (const auto& [key, value] : doc.fields()) {
    if (key == "tool" || key == "type" || key == "message" || key == "label") {
      continue;
    }
    args.set(key, value);
  }
  return args;
}

} // namespace

std::optional<TurnPlan> TryBuildPlanFromPayload(const std::string& user_text, const std::string& payload) {
  if (payload.empty()) {
    return std::nullopt;
  }

  auto doc = TryParseObject(payload);
  if (!doc) {
    return std::nullopt;
  }

  TurnPlan plan;
  plan.source = TurnPlanSource::Payload;
  plan.user_request = util::Trim(user_text);
  if (plan.user_request.empty()) {
    plan.user_request = payload;
  }

  const std::string type = JsonStringOrDefault(*doc, "type");
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

  const std::string tool = JsonStringOrDefault(*doc, "tool");
  if (!tool.empty()) {
    PlannedToolCall call;
    call.name = tool;
    call.arguments = ToolArgumentsFromPayload(*doc);

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
