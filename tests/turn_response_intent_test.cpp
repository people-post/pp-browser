#include "agent/TurnResponseIntent.h"

#include <cassert>
#include <string>

int main() {
  {
    const auto intent = pbr::InferTurnResponseIntent("Show me articles from brief");
    assert(intent.goal == pbr::ResponseGoal::DisplayFeed);
    assert(intent.user_request == "Show me articles from brief");
  }

  {
    const auto intent = pbr::InferTurnResponseIntent("Show me latest news from brief.global");
    assert(intent.goal == pbr::ResponseGoal::DisplayFeed);
  }

  {
    const auto intent = pbr::InferTurnResponseIntent("Summarize art-001");
    assert(intent.goal == pbr::ResponseGoal::Summarize);
  }

  {
    const auto intent = pbr::InferTurnResponseIntent("Why is the Fed raising rates? Also any news today?");
    assert(intent.goal == pbr::ResponseGoal::AnswerQuestion);
  }

  {
    const auto intent = pbr::InferTurnResponseIntent("Show me some today's headlines");
    assert(intent.goal == pbr::ResponseGoal::Headlines);
  }

  {
    const std::string payload = R"({"type":"article","id":"art-001"})";
    const auto intent = pbr::InferTurnResponseIntent("Open art-001", payload);
    assert(intent.goal == pbr::ResponseGoal::Summarize);
  }

  {
    const std::string payload = R"({"tool":"blog_articles","before_id":"art-001","size":10})";
    const auto intent = pbr::InferTurnResponseIntent("More", payload);
    assert(intent.goal == pbr::ResponseGoal::DisplayFeed);
  }

  {
    const auto intent = pbr::InferTurnResponseIntent("help");
    assert(intent.goal == pbr::ResponseGoal::General);
  }

  assert(std::string(pbr::ResponseGoalName(pbr::ResponseGoal::DisplayFeed)) == "display_feed");
  assert(std::string(pbr::ResponseGoalName(pbr::ResponseGoal::AnswerQuestion)) == "answer_question");

  return 0;
}
