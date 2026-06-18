#pragma once

#include <optional>
#include <string>

namespace pbr {

enum class ResponseGoal {
  DisplayFeed,
  Summarize,
  AnswerQuestion,
  Headlines,
  General,
};

struct TurnResponseIntent {
  ResponseGoal goal = ResponseGoal::General;
  std::string user_request;
};

TurnResponseIntent InferTurnResponseIntent(const std::string& user_text,
                                           const std::optional<std::string>& user_payload = std::nullopt);

const char* ResponseGoalName(ResponseGoal goal);

} // namespace pbr
