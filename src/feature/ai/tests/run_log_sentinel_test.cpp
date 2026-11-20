// "Send run logs" (DEV/PLAN-send-run-logs.md, step 1): a question typed to the AI must not reach the log. The question
// carries a unique sentinel and goes through the agent session's paths this target can drive (the brief_AI stream
// answering, the stream failing, the local model pipeline failing), with every logger at the lowest level; no line
// written may hold it. The answer echoes the question, as a model often does.
#include "feature/ai/tests/agent_session_test_support.h"

#include "common/Logger.h"

#include <mutex>

namespace {

using namespace pbr;
using namespace pbr::agent_test;

constexpr const char* kSentinel = "zq-sentinel-9147-雪";

/** Every line any logger writes, at the lowest level (one handler for the process: handlers cannot be removed). */
class SentinelLogCapture : public logging::Handler {
public:
  static SentinelLogCapture& Start() {
    static const std::shared_ptr<SentinelLogCapture> capture = [] {
      auto made = std::make_shared<SentinelLogCapture>();
      logging::getRootLogger().addHandler(made);
      return made;
    }();
    logging::getRootLogger().setLevel(logging::Level::DEBUG);
    logging::setEmitFloor(logging::Level::DEBUG);
    capture->Clear();
    return *capture;
  }
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(message);
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
  }
  std::vector<std::string> LinesWith(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> found;
    for (const std::string& line : lines_) {
      if (line.find(needle) != std::string::npos) {
        found.push_back(line);
      }
    }
    return found;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

class RunLogSentinelTest : public AgentSessionBriefStreamTest {
protected:
  void SetUp() override {
    AgentSessionBriefStreamTest::SetUp();
    capture_ = &SentinelLogCapture::Start();
    logging::getRootLogger().debug << "run-log-sentinel-control";
    ASSERT_EQ(capture_->LinesWith("run-log-sentinel-control").size(), 1u) << "the capture sees DEBUG lines";
  }

  void ExpectNoSentinel() {
    for (const std::string& line : capture_->LinesWith(kSentinel)) {
      ADD_FAILURE() << "user text reached the log: " << line;
    }
  }

  static std::string Question() { return std::string("what does ") + kSentinel + " mean?"; }

  SentinelLogCapture* capture_ = nullptr;
};

TEST_F(RunLogSentinelTest, AnAiQuestionAndItsAnswerAreNeverLogged) {
  Start();
  const std::string echo = std::string("You asked about ") + kSentinel + ".";
  Script({Meta(), Status("search_web", "tool"), Token(echo), Done(echo)}, BriefAiOutcome::Done);
  session_->Submit(Question());
  (void)WaitForTurn();
  ASSERT_EQ(Requests().size(), 1u) << "the question went through the stream";
  ExpectNoSentinel();
}

TEST_F(RunLogSentinelTest, AnAiQuestionIsNotLoggedWhenTheStreamFails) {
  Start();
  Script({Meta(), ErrorEvent("model unavailable", true)}, BriefAiOutcome::Error);
  session_->Submit(Question());
  (void)WaitForTurn();
  ExpectNoSentinel();
}

TEST_F(RunLogSentinelTest, AnAiQuestionIsNotLoggedOnTheLocalModelPath) {
  Start("ollama"); // the completions route on a closed port: the turn fails fast
  session_->Submit(Question());
  (void)WaitForTurn();
  ExpectNoSentinel();
}

} // namespace
