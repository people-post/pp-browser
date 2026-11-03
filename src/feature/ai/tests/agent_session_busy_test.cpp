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
  EXPECT_EQ(session_->SubmitToThread("thread-2", "second"), SubmitOutcome::BusyHere) << "refused, not dropped";
  release = true;

  const auto events = WaitForTurn();
  const auto loadings = Loadings(events);
  ASSERT_EQ(loadings.size(), 2u) << "one turn, one start and one end";
  EXPECT_EQ(loadings.front().thread_id, "thread-1");
  EXPECT_EQ(Requests().size(), 1u);
}

// A turn resets its state before it releases "busy": a send taken the instant IsBusy() turns false keeps its own
// chat (the rest of the old turn's end must not wipe it). The hook sends at exactly that instant.
TEST_F(AgentSessionBusyTest, ASendTakenTheInstantATurnEndsKeepsItsChat) {
  StartWithStore();
  Script({Meta(), Token("x"), Done("x")}, BriefAiOutcome::Done);
  std::atomic<bool> sent{false};
  AgentSession::SetTurnReleasedHookForTests([this, &sent] {
    if (!sent.exchange(true)) {
      EXPECT_EQ(session_->SubmitToThread("thread-2", "second"), SubmitOutcome::Started);
    }
  });
  session_->SubmitToThread("thread-1", "first");
  (void)WaitForTurn();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while ((!sent || session_->IsBusy()) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  AgentSession::SetTurnReleasedHookForTests(nullptr);
  ASSERT_TRUE(sent);
  ASSERT_FALSE(session_->IsBusy());

  std::vector<std::string> second_turn; // sender of each message stored for thread-2
  for (const ThreadMessage& message : store_.messages) {
    EXPECT_FALSE(message.thread_id.empty()) << "a message lost its chat: " << message.text;
    if (message.thread_id == "thread-2") {
      second_turn.push_back(message.sender_contact_id);
    }
  }
  EXPECT_EQ(second_turn, (std::vector<std::string>{kLocalSelfContactId, kAiAssistantContactId}));
}

// An error names the chat of its turn, so the UI writes it there only (never into Home's last answer).
TEST_F(AgentSessionBusyTest, AnErrorNamesTheChatOfItsTurn) {
  StartWithStore();
  Script({Meta(), ErrorEvent("model unavailable", true)}, BriefAiOutcome::Error);
  session_->SubmitToThread("thread-x", "q");
  const auto errors = Of(WaitForTurn(), AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0].thread_id, "thread-x");
  ASSERT_FALSE(store_.messages.empty());
  EXPECT_EQ(errors[0].entry_id, store_.messages.front().id) << "the turn's question";
}

TEST_F(AgentSessionBusyTest, AHomeErrorNamesNoChatAndItsOwnEntry) {
  Start();
  Script({Meta(), ErrorEvent("model unavailable", true)}, BriefAiOutcome::Error);
  session_->Submit("q");
  const auto errors = Of(WaitForTurn(), AgentEventType::Error);
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_TRUE(errors[0].thread_id.empty());
  ASSERT_FALSE(session_->conversation().Entries().empty());
  EXPECT_EQ(errors[0].entry_id, session_->conversation().Entries().back().id);
}

} // namespace
