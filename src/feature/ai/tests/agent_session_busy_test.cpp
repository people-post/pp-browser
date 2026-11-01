// The "busy" signal the chat screen builds its per-thread thinking state on (B112): LoadingChanged(true) names the
// thread of the turn, LoadingChanged(false) always comes however the turn ends, IsBusy() follows the turn.
#include "feature/ai/tests/agent_session_test_support.h"

namespace {

using namespace pbr;
using namespace pbr::agent_test;

class AgentSessionBusyTest : public AgentSessionBriefStreamTest {
protected:
  /** A thread turn needs a thread store (the question and the answer are stored in the thread). */
  void StartWithStore() {
    Start();
    session_->SetThreadStore(&store_);
  }

  /** A stream that sends one token, then waits until `release` is set or the turn is cancelled. */
  void ScriptHeldStream(std::atomic<bool>& started, std::atomic<bool>& release) {
    session_->SetBriefAiStream([this, &started, &release](const BriefAiRequest& request,
                                                          const std::function<void(const BriefAiEvent&)>& on_event,
                                                          const std::atomic<bool>& cancel) -> Roe<BriefAiOutcome> {
      Record(request);
      on_event(Meta());
      on_event(Token("part"));
      started = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!cancel.load() && !release.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (cancel.load()) {
        return BriefAiOutcome::Cancelled;
      }
      on_event(Done("part"));
      return BriefAiOutcome::Done;
    });
  }

  static void WaitUntil(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(flag.load());
  }

  MemoryThreadStore store_;

  static std::vector<AgentEvent> Loadings(const std::vector<AgentEvent>& events) {
    return Of(events, AgentEventType::LoadingChanged);
  }
};

TEST_F(AgentSessionBusyTest, StartCarriesTheThreadOfTheTurn) {
  StartWithStore();
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("thread-7", "hello there");
  const auto events = WaitForTurn();

  const auto loadings = Loadings(events);
  ASSERT_GE(loadings.size(), 2u);
  EXPECT_TRUE(loadings.front().loading);
  EXPECT_EQ(loadings.front().thread_id, "thread-7");
  EXPECT_FALSE(loadings.back().loading);
  EXPECT_FALSE(session_->IsBusy());
}

TEST_F(AgentSessionBusyTest, AnotherThreadsTurnNamesItsOwnThread) {
  StartWithStore();
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("thread-1", "first");
  auto events = WaitForTurn();
  ASSERT_FALSE(Loadings(events).empty());
  EXPECT_EQ(Loadings(events).front().thread_id, "thread-1");

  session_->SubmitToThread("thread-2", "second");
  events = WaitForTurn();
  ASSERT_FALSE(Loadings(events).empty());
  EXPECT_EQ(Loadings(events).front().thread_id, "thread-2");
}

TEST_F(AgentSessionBusyTest, IsBusyHoldsDuringTheTurnAndEndsWithIt) {
  StartWithStore();
  EXPECT_FALSE(session_->IsBusy());
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  ScriptHeldStream(started, release);
  session_->SubmitToThread("t1", "q");
  EXPECT_TRUE(session_->IsBusy()) << "set synchronously by Submit, before any event is polled";
  WaitUntil(started);
  EXPECT_TRUE(session_->IsBusy());

  release = true;
  const auto events = WaitForTurn();
  ASSERT_FALSE(Loadings(events).empty());
  EXPECT_FALSE(Loadings(events).back().loading);
  EXPECT_FALSE(session_->IsBusy());
}

TEST_F(AgentSessionBusyTest, NormalEndPushesLoadingFalseLast) {
  StartWithStore();
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "q");
  const auto events = WaitForTurn();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
  EXPECT_EQ(Of(events, AgentEventType::AssistantReady).size(), 1u);
}

TEST_F(AgentSessionBusyTest, StreamErrorPushesLoadingFalse) {
  StartWithStore();
  Script({Meta(), ErrorEvent("model unavailable", true)}, BriefAiOutcome::Error);
  session_->SubmitToThread("t1", "q");
  const auto events = WaitForTurn();
  EXPECT_FALSE(Of(events, AgentEventType::Error).empty());
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
  EXPECT_FALSE(session_->IsBusy());
}

TEST_F(AgentSessionBusyTest, CancelPushesLoadingFalse) {
  StartWithStore();
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  ScriptHeldStream(started, release);
  session_->SubmitToThread("t1", "q");
  WaitUntil(started);
  EXPECT_TRUE(session_->IsBusy());

  session_->Cancel();
  EXPECT_FALSE(session_->IsBusy());
  const auto events = WaitForTurn();
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().type, AgentEventType::LoadingChanged);
  EXPECT_FALSE(events.back().loading);
  EXPECT_TRUE(Of(events, AgentEventType::Error).empty());
}

TEST_F(AgentSessionBusyTest, ASecondSendWhileBusyIsRefusedAndKeepsTheFirstTurnsThread) {
  StartWithStore();
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
  ScriptHeldStream(started, release);
  session_->SubmitToThread("thread-1", "first");
  WaitUntil(started);
  session_->SubmitToThread("thread-2", "second"); // dropped with a warning: the UI queues instead
  release = true;

  const auto events = WaitForTurn();
  const auto loadings = Loadings(events);
  ASSERT_EQ(loadings.size(), 2u) << "one turn, one start and one end";
  EXPECT_EQ(loadings.front().thread_id, "thread-1");
  EXPECT_EQ(Requests().size(), 1u);
}

} // namespace
