#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>
#include <vector>

namespace pbr {

enum class TurnPlanSource {
  Payload,
  Planner,
};

enum class ResponseGoal {
  DisplayFeed,
  Summarize,
  AnswerQuestion,
  Headlines,
  PeopleDiscovery,
  General,
};

enum class RenderMode {
  Blocks,
  PeopleList,
};

struct PlannedToolCall {
  std::string name;
  Object arguments;
};

struct TurnPlan {
  TurnPlanSource source = TurnPlanSource::Planner;
  ResponseGoal response_goal = ResponseGoal::General;
  std::vector<PlannedToolCall> tools;
  RenderMode render_mode = RenderMode::Blocks;
  std::string synthesis_hints;
  std::string user_request;
};

const char* TurnPlanSourceName(TurnPlanSource source);
const char* ResponseGoalName(ResponseGoal goal);
const char* RenderModeName(RenderMode mode);

ResponseGoal ParseResponseGoal(const std::string& name);
RenderMode ParseRenderMode(const std::string& name);

Roe<TurnPlan> ParseTurnPlanJson(const Object& doc, TurnPlanSource source);
Roe<TurnPlan> ParseTurnPlanFromLlmOutput(const std::string& llm_output, TurnPlanSource source);
Roe<TurnPlan> ValidateTurnPlan(TurnPlan plan, const std::vector<std::string>& allowed_tools);

Object TurnPlanToJson(const TurnPlan& plan);

} // namespace pbr
