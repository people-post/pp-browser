#include "feature/ai/BriefTurnRunner.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using pbr::BriefAiEvent;
using pbr::BriefAiOutcome;

BriefAiEvent Token(const std::string& delta) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Token;
  event.delta = delta;
  return event;
}

BriefAiEvent Done(const std::string& response, const std::string& finish = "stop") {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Done;
  event.response = response;
  event.finish = finish;
  event.sources.push_back(pbr::BriefAiSource{.title = "T", .url = "https://x", .kind = "web"});
  return event;
}

BriefAiEvent Status(const std::string& tool, const std::string& phase) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Status;
  event.tool = tool;
  event.phase = phase;
  event.query = "secret";
  return event;
}

// Records every sink call so tests can assert on order and terminal uniqueness.
struct Recorder {
  std::vector<std::string> deltas;
  std::vector<std::string> statuses;
  int done = 0, handoff = 0, error = 0, cancelled = 0;
  std::string response, finish, error_message, partial;
  size_t sources = 0;
  bool retryable = false;

  int Terminals() const { return done + handoff + error + cancelled; }

  pbr::BriefTurnSinks Sinks() {
    pbr::BriefTurnSinks sinks;
    sinks.on_delta = [this](const std::string& text) { deltas.push_back(text); };
    sinks.on_status = [this](const std::string& tool, const std::string& phase, const std::string&) {
      statuses.push_back(tool + ":" + phase);
    };
    sinks.on_done = [this](const std::string& r, const std::string& f, const std::vector<pbr::BriefAiSource>& s) {
      ++done;
      response = r;
      finish = f;
      sources = s.size();
    };
    sinks.on_handoff = [this]() { ++handoff; };
    sinks.on_error = [this](const std::string& m, bool r, const std::string& p) {
      ++error;
      error_message = m;
      retryable = r;
      partial = p;
    };
    sinks.on_cancelled = [this](const std::string& p) {
      ++cancelled;
      partial = p;
    };
    return sinks;
  }
};

pbr::BriefAiStreamFn Script(std::vector<BriefAiEvent> events, pbr::Roe<BriefAiOutcome> outcome) {
  return [events = std::move(events), outcome = std::move(outcome)](
             const pbr::BriefAiRequest&, const std::function<void(const BriefAiEvent&)>& on_event,
             const std::atomic<bool>&) {
    for (const BriefAiEvent& event : events) {
      on_event(event);
    }
    return outcome;
  };
}

void RunScript(const pbr::BriefAiStreamFn& stream, Recorder& rec) {
  const std::atomic<bool> cancel{false};
  pbr::BriefTurnRunner::Run(stream, pbr::BriefAiRequest{}, cancel, rec.Sinks());
}

} // namespace

TEST(BriefTurnRunnerTest, NormalAnswerAccumulatesDeltasAndFiresDone) {
  Recorder rec;
  RunScript(Script({Token("Hel"), Token("lo"), Done("Hello", "stop")}, BriefAiOutcome::Done), rec);
  EXPECT_EQ(rec.deltas, (std::vector<std::string>{"Hel", "Hello"}));
  EXPECT_EQ(rec.done, 1);
  EXPECT_EQ(rec.response, "Hello");
  EXPECT_EQ(rec.finish, "stop");
  EXPECT_EQ(rec.sources, 1u);
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, EmptyDoneResponseUsesAccumulatedText) {
  Recorder rec;
  RunScript(Script({Token("a"), Token("b"), Done("")}, BriefAiOutcome::Done), rec);
  EXPECT_EQ(rec.response, "ab");
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, HandoffFiresOnlyHandoff) {
  Recorder rec;
  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  RunScript(Script({handoff}, BriefAiOutcome::Handoff), rec);
  EXPECT_TRUE(rec.deltas.empty());
  EXPECT_EQ(rec.handoff, 1);
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, ErrorEventAfterTokensCarriesPartialText) {
  Recorder rec;
  BriefAiEvent error;
  error.type = BriefAiEvent::Type::Error;
  error.message = "upstream failed";
  error.retryable = true;
  RunScript(Script({Token("one "), Token("two"), error}, BriefAiOutcome::Error), rec);
  EXPECT_EQ(rec.error, 1);
  EXPECT_EQ(rec.error_message, "upstream failed");
  EXPECT_TRUE(rec.retryable);
  EXPECT_EQ(rec.partial, "one two");
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, InterruptedStreamIsRetryableErrorWithPartial) {
  Recorder rec;
  RunScript(Script({Token("par"), Token("tial")},
             pbr::AppError::Network(pbr::Err::Network::HttpError, "answer interrupted")),
      rec);
  EXPECT_EQ(rec.error, 1);
  EXPECT_TRUE(rec.retryable);
  EXPECT_EQ(rec.partial, "partial");
  EXPECT_FALSE(rec.error_message.empty());
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, CancelledFiresCancelledWithPartial) {
  Recorder rec;
  RunScript(Script({Token("half")}, BriefAiOutcome::Cancelled), rec);
  EXPECT_EQ(rec.cancelled, 1);
  EXPECT_EQ(rec.partial, "half");
  EXPECT_EQ(rec.Terminals(), 1);
}

TEST(BriefTurnRunnerTest, StatusEventsReachOnStatus) {
  Recorder rec;
  RunScript(Script({Status("web_search", "running"), Status("web_search", "done"), Token("x"), Done("x")},
             BriefAiOutcome::Done),
      rec);
  EXPECT_EQ(rec.statuses, (std::vector<std::string>{"web_search:running", "web_search:done"}));
  EXPECT_EQ(rec.Terminals(), 1);
}
