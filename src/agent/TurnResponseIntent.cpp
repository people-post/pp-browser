#include "agent/TurnResponseIntent.h"

#include "agent/SearchIntent.h"

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

std::string Lower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool ContainsAny(const std::string& text, const std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (Contains(text, needle)) {
      return true;
    }
  }
  return false;
}

std::optional<ResponseGoal> GoalFromPayload(const std::string& payload) {
  const nlohmann::json doc = nlohmann::json::parse(payload, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return std::nullopt;
  }

  const std::string type = doc.value("type", "");
  if (type == "article") {
    return ResponseGoal::Summarize;
  }

  const std::string tool = doc.value("tool", "");
  if (tool == "blog_articles" || Contains(tool, "article") || Contains(tool, "feed")) {
    return ResponseGoal::DisplayFeed;
  }

  return std::nullopt;
}

bool WantsSummarize(const std::string& text_lower) {
  return ContainsAny(text_lower, {"summarize", "summary", "tl;dr", "tldr", "brief me on", "short version",
                                  "in short", "recap"});
}

bool WantsDisplayFeed(const std::string& user_message) {
  return WantsArticleFeedRequest(user_message);
}

bool WantsAnswerQuestion(const std::string& text_lower) {
  return ContainsAny(text_lower, {"why ", "why?", "explain", "how does", "how do ", "what does it mean",
                                  "compare", "difference between", "should i", "pros and cons", "impact of",
                                  "what caused", "what happens if"});
}

} // namespace

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
  case ResponseGoal::General:
    return "general";
  }
  return "general";
}

TurnResponseIntent InferTurnResponseIntent(const std::string& user_text,
                                           const std::optional<std::string>& user_payload) {
  TurnResponseIntent intent;
  intent.user_request = Trim(user_text);
  if (intent.user_request.empty() && user_payload && !user_payload->empty()) {
    intent.user_request = *user_payload;
  }

  if (user_payload && !user_payload->empty()) {
    if (const auto goal = GoalFromPayload(*user_payload)) {
      intent.goal = *goal;
      return intent;
    }
  }

  const std::string text_lower = Lower(intent.user_request);

  if (WantsSummarize(text_lower)) {
    intent.goal = ResponseGoal::Summarize;
    return intent;
  }

  if (WantsDisplayFeed(intent.user_request)) {
    intent.goal = ResponseGoal::DisplayFeed;
    return intent;
  }

  if (WantsAnswerQuestion(text_lower)) {
    intent.goal = ResponseGoal::AnswerQuestion;
    return intent;
  }

  if (WantsNewsHeadlines(intent.user_request)) {
    intent.goal = ResponseGoal::Headlines;
    return intent;
  }

  intent.goal = ResponseGoal::General;
  return intent;
}

} // namespace pbr
