// "Busy" belongs to one thread (B112): the real AgentTurnController is driven with a fake chat screen. What is
// checked: what the open thread shows as it switches, the queue of AI sends of other threads and how it drains,
// the queued bubble, the stop key, and the fate of a held send.
#include "gui/chat/AgentTurnController.h"

#include <gtest/gtest.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

namespace pbr {
namespace {

class FakeHost : public AgentTurnHost {
public:
  std::string active_thread;
  std::vector<bool> activity; // ShellSetActivity(visible, ...) calls, in order
  std::deque<QueuedSendOutcome> outcomes; // scripted; NotStarted when it runs dry
  std::vector<QueuedAgentSend> handed_over;
  std::vector<std::pair<std::string, std::string>> put_back; // (thread, text) handed back to that thread's composer
  struct Sent {
    std::string text;
    bool from_home_chip = false;
  };
  std::vector<Sent> sent;
  SessionStore store;

  ConversationsFacade* Facade() const override { return nullptr; }
  bool MessagingIsReady() const override { return false; }
  bool MessagingInitialized() const override { return false; }
  bool MessagingReady() const override { return false; }
  const std::string& ActiveThreadId() const override { return active_thread; }
  ShellChromeSnapshot ChromeSnapshot() const override { return {}; }
  bool InAiComposerContext() const override { return true; }
  void SyncDisplayFromThread() override {}
  void SyncShellSessions() override {}
  void SyncComposerInputState() override {}
  void UpdateSidebarPreview(const std::string&) override {}
  bool EnsureHomeOutboundSession(const std::string&) override { return true; }
  void SendUserText(const std::string& text, std::optional<std::string>, bool from_home_chip) override {
    sent.push_back({text, from_home_chip});
  }
  void ShellSetActivity(bool visible, const ui::String&) override { activity.push_back(visible); }
  void ShowAttachChoice(std::function<void()>, std::function<void()>) override {}
  void RegisterMessagingTools(ToolRegistry&) override {}
  SessionStore& Store() override { return store; }
  void HandleLocalAction(const std::string&, const std::optional<std::string>&, const std::string&) override {}
  void PutBackDraft(const std::string& thread_id, const std::string& text) override {
    put_back.push_back({thread_id, text});
  }
  QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend& item) override {
    handed_over.push_back(item);
    if (outcomes.empty()) {
      return QueuedSendOutcome::NotStarted;
    }
    const QueuedSendOutcome outcome = outcomes.front();
    outcomes.pop_front();
    return outcome;
  }
};

AgentEvent Loading(bool loading, const std::string& thread = {}) {
  AgentEvent event;
  event.type = AgentEventType::LoadingChanged;
  event.loading = loading;
  event.thread_id = thread;
  return event;
}

QueuedAgentSend Queued(const std::string& thread, const std::string& text = "question") {
  QueuedAgentSend item;
  item.thread_id = thread;
  item.text = text;
  return item;
}

std::vector<std::string> Threads(const std::vector<QueuedAgentSend>& items) {
  std::vector<std::string> out;
  for (const QueuedAgentSend& item : items) {
    out.push_back(item.thread_id);
  }
  return out;
}

class AgentTurnBusyTest : public ::testing::Test {
protected:
  AgentTurnBusyTest() {
    // A mock-reply setup (no base_url): "ready" then only needs the agent session to exist.
    host_.store.Initialize(BootstrapResult{});
    host_.store.Mutable().config.llm.base_url.clear();
  }

  void Open(const std::string& thread) {
    host_.active_thread = thread;
    controller_.RefreshBusyForActiveThread();
  }

  /** The agent session port: exists, counts how often Stop reached it. */
  void BindSession() {
    AgentUiPorts ports;
    ports.has_session = [] { return true; };
    ports.cancel = [this] { ++cancel_calls_; };
    controller_.BindPorts(std::move(ports));
  }

  FakeHost host_;
  ChatScreenState chat_;
  std::map<std::string, std::vector<std::string>> chat_links_;
  ChatWidgetHost widgets_;
  bool ws_active_ = false;
  ui::String ws_title_, ws_subtitle_, ws_rml_;
  TurnWidgetState ws_widget_;
  WorkingSetController working_set_{{ws_active_, ws_title_, ws_subtitle_, ws_rml_, ws_widget_}};
  ShellFeedbackPorts feedback_;
  AgentTurnController controller_{host_, chat_, chat_links_, widgets_, working_set_, feedback_};
  int cancel_calls_ = 0;
};

// --- what the open thread shows ----------------------------------------------------------------

TEST_F(AgentTurnBusyTest, ThinkingShowsOnlyInTheThreadWhoseTurnRuns) {
  Open("A");
  EXPECT_FALSE(chat_.loading);
  controller_.MarkAgentBusy("A");
  EXPECT_TRUE(chat_.loading);

  Open("B");
  EXPECT_FALSE(chat_.loading) << "another thread keeps the send key";
  Open("A");
  EXPECT_TRUE(chat_.loading);

  controller_.MarkAgentIdle();
  EXPECT_FALSE(chat_.loading);
}

TEST_F(AgentTurnBusyTest, StatusBarActivityFollowsTheOpenThread) {
  Open("A");
  controller_.MarkAgentBusy("A");
  EXPECT_TRUE(host_.activity.back());
  Open("B");
  EXPECT_FALSE(host_.activity.back());
  Open("A");
  EXPECT_TRUE(host_.activity.back());
  controller_.MarkAgentIdle();
  EXPECT_FALSE(host_.activity.back());
}

TEST_F(AgentTurnBusyTest, UnknownThreadShowsThinkingNowhereButAiSendsStillQueue) {
  // Kenneth 2026-10-28: "thinking" belongs to the conversation that is answering; when that is unknown it is shown
  // nowhere, while every AI send still queues (DecideSend treats an unknown thread as busy).
  controller_.MarkAgentBusy("");
  for (const char* thread : {"A", "B", "C"}) {
    Open(thread);
    EXPECT_FALSE(chat_.loading) << thread;
    EXPECT_EQ(DecideSend(true, controller_.Busy(), thread, false), SendDecision::Queue) << thread;
  }
  controller_.MarkAgentIdle();
  EXPECT_FALSE(chat_.loading);
}

TEST_F(AgentTurnBusyTest, LoadingChangedCarriesTheThreadIn) {
  Open("A");
  controller_.HandleAgentEvent(Loading(true, "A"));
  EXPECT_TRUE(chat_.loading);
  EXPECT_TRUE(controller_.Busy().BusyIn("A"));
  Open("B");
  EXPECT_FALSE(chat_.loading);
  EXPECT_TRUE(controller_.Busy().BusyElsewhere("B"));

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_FALSE(controller_.Busy().busy);
  EXPECT_FALSE(chat_.loading);
}

TEST_F(AgentTurnBusyTest, AnEventWithoutAThreadKeepsTheThreadHandedOver) {
  // The send marks busy synchronously with its thread; the late LoadingChanged(true) may not name one.
  Open("A");
  controller_.MarkAgentBusy("A");
  controller_.HandleAgentEvent(Loading(true));
  EXPECT_TRUE(controller_.Busy().BusyIn("A"));
  EXPECT_FALSE(controller_.Busy().BusyIn("B"));
}

// --- the queue ---------------------------------------------------------------------------------

TEST_F(AgentTurnBusyTest, IdleEventDrainsTheQueue) {
  Open("A");
  controller_.HandleAgentEvent(Loading(true, "A"));
  ASSERT_TRUE(controller_.QueueSend(Queued("B")));
  EXPECT_TRUE(host_.handed_over.empty()) << "nothing is handed over while the AI is busy";

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"}));
  EXPECT_EQ(controller_.QueuedCount(), 0u);
}

TEST_F(AgentTurnBusyTest, ATurnThatStartedHoldsBackTheRest) {
  host_.outcomes = {QueuedSendOutcome::Started};
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.QueueSend(Queued("C"));

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"})) << "C waits for B's turn to end";
  EXPECT_EQ(controller_.QueuedCount(), 1u);
  EXPECT_TRUE(controller_.HasQueued("C"));
  EXPECT_FALSE(controller_.HasQueued("B"));
}

TEST_F(AgentTurnBusyTest, ASendThatDidNotStartIsSkippedAndTheNextOneRuns) {
  host_.outcomes = {QueuedSendOutcome::NotStarted, QueuedSendOutcome::Started};
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.QueueSend(Queued("C"));

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B", "C"}));
  EXPECT_EQ(controller_.QueuedCount(), 0u);
}

TEST_F(AgentTurnBusyTest, AQueueOfSendsThatNeverStartStillEmpties) {
  // outcomes run dry -> NotStarted for each
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.QueueSend(Queued("C"));
  controller_.QueueSend(Queued("D"));

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B", "C", "D"}));
  EXPECT_EQ(controller_.QueuedCount(), 0u);
}

TEST_F(AgentTurnBusyTest, ARefusedSendGoesBackToTheHeadInOrder) {
  host_.outcomes = {QueuedSendOutcome::Busy};
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.QueueSend(Queued("C"));

  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"})) << "C is not handed over behind a refusal";
  EXPECT_EQ(controller_.QueuedCount(), 2u);

  // The next idle event offers B first again, then C.
  host_.handed_over.clear();
  host_.outcomes = {QueuedSendOutcome::NotStarted, QueuedSendOutcome::NotStarted};
  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B", "C"}));
}

TEST_F(AgentTurnBusyTest, TwoIdleEventsInARowHandOverOnlyOnce) {
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.HandleAgentEvent(Loading(false));
  controller_.HandleAgentEvent(Loading(false));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"}));
}

TEST_F(AgentTurnBusyTest, AnErrorWaitsForTheTurnsEndBeforeDraining) {
  Open("A");
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));

  AgentEvent error;
  error.type = AgentEventType::Error;
  error.message = "boom";
  controller_.HandleAgentEvent(error);
  EXPECT_TRUE(controller_.Busy().busy) << "the turn's own LoadingChanged(false) ends it";
  EXPECT_TRUE(host_.handed_over.empty());

  AgentEvent ended;
  ended.type = AgentEventType::LoadingChanged;
  ended.loading = false;
  ended.thread_id = "A";
  controller_.HandleAgentEvent(ended);
  EXPECT_FALSE(chat_.loading);
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"}));
}

TEST_F(AgentTurnBusyTest, ALateEndOfAnOlderTurnDoesNotEndTheQueuedOne) {
  Open("B");
  controller_.MarkAgentBusy("B"); // the queued turn that started after A's end
  AgentEvent late;
  late.type = AgentEventType::LoadingChanged;
  late.loading = false;
  late.thread_id = "A";
  controller_.HandleAgentEvent(late);
  EXPECT_TRUE(controller_.Busy().BusyIn("B"));
  EXPECT_TRUE(chat_.loading);
}

TEST_F(AgentTurnBusyTest, QueuingWhileIdleSendsAtOnce) {
  // The AI went idle while the "@ai+" confirm was open: the queued send must not wait for another turn's end.
  host_.outcomes.push_back(QueuedSendOutcome::Started);
  controller_.QueueSend(Queued("B"));
  EXPECT_EQ(Threads(host_.handed_over), (std::vector<std::string>{"B"}));
  EXPECT_EQ(controller_.QueuedCount(), 0u);
}

TEST_F(AgentTurnBusyTest, NothingIsHandedOverWhileStillBusy) {
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B"));
  controller_.DrainQueue();
  EXPECT_TRUE(host_.handed_over.empty());
  EXPECT_EQ(controller_.QueuedCount(), 1u);
}

// --- the queued bubble -------------------------------------------------------------------------

TEST_F(AgentTurnBusyTest, QueuedBubbleShowsInItsOwnThreadOnly) {
  Open("B");
  controller_.MarkAgentBusy("A");
  EXPECT_FALSE(chat_.has_queued);

  ASSERT_TRUE(controller_.QueueSend(Queued("B", "what is the news")));
  EXPECT_TRUE(chat_.has_queued);
  EXPECT_STREQ(chat_.queued_text.c_str(), "what is the news");

  Open("A");
  EXPECT_FALSE(chat_.has_queued) << "the bubble belongs to B";
  EXPECT_STREQ(chat_.queued_text.c_str(), "");
  Open("B");
  EXPECT_TRUE(chat_.has_queued);
}

TEST_F(AgentTurnBusyTest, QueuingForAnotherThreadShowsNoBubbleHere) {
  Open("A");
  controller_.MarkAgentBusy("A");
  ASSERT_TRUE(controller_.QueueSend(Queued("B")));
  EXPECT_FALSE(chat_.has_queued);
  EXPECT_STREQ(chat_.queued_text.c_str(), "");
}

TEST_F(AgentTurnBusyTest, TakingTheQueuedSendRemovesTheBubble) {
  Open("B");
  controller_.MarkAgentBusy("A");
  controller_.QueueSend(Queued("B", "text"));
  const auto taken = controller_.TakeQueued("B");
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->text, "text");
  EXPECT_FALSE(chat_.has_queued);
  EXPECT_STREQ(chat_.queued_text.c_str(), "");
  EXPECT_FALSE(controller_.HasQueued("B"));
  EXPECT_FALSE(controller_.TakeQueued("B").has_value());
}

TEST_F(AgentTurnBusyTest, ASecondQueuedSendForTheSameThreadIsRefused) {
  controller_.MarkAgentBusy("A");
  EXPECT_TRUE(controller_.QueueSend(Queued("B", "first")));
  EXPECT_FALSE(controller_.QueueSend(Queued("B", "second")));
  EXPECT_EQ(controller_.QueuedCount(), 1u);
}

// --- the stop key ------------------------------------------------------------------------------

TEST_F(AgentTurnBusyTest, StopReachesTheSessionOnlyFromTheThreadWhoseTurnRuns) {
  BindSession();
  controller_.MarkAgentBusy("A");

  Open("B");
  controller_.OnStopTurn();
  EXPECT_EQ(cancel_calls_, 0);

  Open("A");
  controller_.OnStopTurn();
  EXPECT_EQ(cancel_calls_, 1);
}

TEST_F(AgentTurnBusyTest, StopWithNothingRunningDoesNothing) {
  BindSession();
  Open("A");
  controller_.OnStopTurn();
  EXPECT_EQ(cancel_calls_, 0);
}

// --- a held send -------------------------------------------------------------------------------

HeldAgentSend::Send Held(const std::string& thread, bool chip = false) {
  return {.text = "held question", .payload = std::nullopt, .from_home_chip = chip, .thread_id = thread};
}

TEST(HeldSendFateBusyTest, OnlyAReplyInTheSendsOwnThreadHoldsItBack) {
  // The third argument is now "a turn runs in the send's own thread".
  EXPECT_EQ(HeldSendFateOf(Held("B"), "B", false), HeldSendFate::Send);
  EXPECT_EQ(HeldSendFateOf(Held("B"), "B", true), HeldSendFate::BackToDraft);
  EXPECT_EQ(HeldSendFateOf(Held("B", true), "B", true), HeldSendFate::Drop);
  EXPECT_EQ(HeldSendFateOf(Held("B"), "A", false), HeldSendFate::BackToDraft);
}

TEST_F(AgentTurnBusyTest, HeldSendGoesOutWhenOnlyAnotherThreadIsBusy) {
  BindSession();
  Open("B");
  controller_.MarkAgentBusy("A");
  controller_.HoldSend(Held("B"));
  controller_.Tick();
  ASSERT_EQ(host_.sent.size(), 1u) << "SendUserText decides (queues) it; the held slot does not block on A's turn";
  EXPECT_EQ(host_.sent[0].text, "held question");
}

TEST_F(AgentTurnBusyTest, HeldSendGoesBackToTheDraftWhenItsOwnThreadIsBusy) {
  BindSession();
  Open("B");
  controller_.MarkAgentBusy("B");
  controller_.HoldSend(Held("B"));
  controller_.Tick();
  EXPECT_TRUE(host_.sent.empty());
  ASSERT_EQ(host_.put_back.size(), 1u);
  EXPECT_EQ(host_.put_back[0], (std::pair<std::string, std::string>{"B", "held question"}));
}

TEST_F(AgentTurnBusyTest, HeldChipIsDroppedWhenItsOwnThreadIsBusy) {
  BindSession();
  Open("B");
  controller_.MarkAgentBusy("B");
  controller_.HoldSend(Held("B", /*chip=*/true));
  controller_.Tick();
  EXPECT_TRUE(host_.sent.empty());
  EXPECT_TRUE(host_.put_back.empty());
}

TEST_F(AgentTurnBusyTest, HeldSendOfAnotherThreadThanTheOpenOneGoesBackToItsOwnDraft) {
  BindSession();
  Open("A");
  controller_.HoldSend(Held("B"));
  controller_.Tick();
  EXPECT_TRUE(host_.sent.empty());
  // Into B's composer, not into the open A's (one Enter would have sent B's text to A).
  ASSERT_EQ(host_.put_back.size(), 1u);
  EXPECT_EQ(host_.put_back[0].first, "B");
  EXPECT_STREQ(chat_.draft.c_str(), "");
}

TEST_F(AgentTurnBusyTest, AnotherChatsTurnIsSaidAboveTheComposer) {
  controller_.MarkAgentBusy("A");
  Open("B");
  controller_.RefreshBusyForActiveThread();
  EXPECT_TRUE(chat_.busy_elsewhere);
  EXPECT_FALSE(chat_.loading);
  Open("A");
  controller_.RefreshBusyForActiveThread();
  EXPECT_FALSE(chat_.busy_elsewhere) << "in its own chat it is \"thinking\", not \"another chat\"";
  controller_.MarkAgentIdle();
  Open("B");
  controller_.RefreshBusyForActiveThread();
  EXPECT_FALSE(chat_.busy_elsewhere);
}

} // namespace
} // namespace pbr
