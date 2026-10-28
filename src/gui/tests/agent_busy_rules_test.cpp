#include "gui/chat/AgentBusyRules.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

AgentBusyState Idle() { return {}; }
AgentBusyState BusyIn(const std::string& thread) { return {.busy = true, .thread_id = thread}; }

TEST(DecideSendTest, MessagesThatDoNotUseTheAiAlwaysGoOut) {
  for (const AgentBusyState& state : {Idle(), BusyIn("A"), BusyIn("B"), BusyIn("")}) {
    EXPECT_EQ(DecideSend(false, state, "A", false), SendDecision::Send);
  }
  // Even with a queued AI send waiting in the target thread.
  EXPECT_EQ(DecideSend(false, BusyIn("B"), "A", true), SendDecision::Send);
}

TEST(DecideSendTest, AiSendWhileIdleGoesOut) {
  EXPECT_EQ(DecideSend(true, Idle(), "A", false), SendDecision::Send);
}

TEST(DecideSendTest, AiSendWhileAnotherThreadRunsIsQueued) {
  EXPECT_EQ(DecideSend(true, BusyIn("B"), "A", false), SendDecision::Queue);
}

TEST(DecideSendTest, AiSendInTheRunningThreadIsBlocked) {
  EXPECT_EQ(DecideSend(true, BusyIn("A"), "A", false), SendDecision::BlockedHere);
}

TEST(DecideSendTest, BusyWithoutAThreadQueuesInsteadOfDropping) {
  EXPECT_EQ(DecideSend(true, BusyIn(""), "A", false), SendDecision::Queue);
}

TEST(DecideSendTest, TargetWithAQueuedSendIsAlreadyQueued) {
  EXPECT_EQ(DecideSend(true, BusyIn("B"), "A", true), SendDecision::AlreadyQueued);
  EXPECT_EQ(DecideSend(true, Idle(), "A", true), SendDecision::AlreadyQueued);
  EXPECT_EQ(DecideSend(true, BusyIn(""), "A", true), SendDecision::AlreadyQueued);
}

TEST(AgentBusyStateTest, BusyInAndBusyElsewhere) {
  EXPECT_FALSE(Idle().BusyIn("A"));
  EXPECT_FALSE(Idle().BusyElsewhere("A"));

  EXPECT_TRUE(BusyIn("A").BusyIn("A"));
  EXPECT_FALSE(BusyIn("A").BusyIn("B"));
  EXPECT_FALSE(BusyIn("A").BusyElsewhere("A"));
  EXPECT_TRUE(BusyIn("A").BusyElsewhere("B"));

  // Unknown thread: busy in every thread, elsewhere in none.
  EXPECT_TRUE(BusyIn("").BusyIn("A"));
  EXPECT_TRUE(BusyIn("").BusyIn(""));
  EXPECT_FALSE(BusyIn("").BusyElsewhere("A"));

  // A stale thread id on an idle state means nothing.
  AgentBusyState stale{.busy = false, .thread_id = "A"};
  EXPECT_FALSE(stale.BusyIn("A"));
}

TEST(DecideEscapeTest, SinglePressCancelsQueuedOrDoesNothing) {
  EXPECT_EQ(DecideEscape(1000, std::nullopt, true, false), EscapeAction::CancelQueued);
  EXPECT_EQ(DecideEscape(1000, std::nullopt, false, false), EscapeAction::None);
  // A turn running here is not stopped by a single press: the press only arms the stop (and is used up, so it does
  // not leave the chat, which would make the second press impossible).
  EXPECT_EQ(DecideEscape(1000, std::nullopt, false, true), EscapeAction::Armed);
}

TEST(DecideEscapeTest, SecondPressInsideTheWindowStopsTheTurnHere) {
  EXPECT_EQ(DecideEscape(1300, 1000, false, true), EscapeAction::StopTurn);
  EXPECT_EQ(DecideEscape(1000 + kDoubleEscapeWindowMs, 1000, false, true), EscapeAction::StopTurn); // exactly 500 ms
  EXPECT_EQ(DecideEscape(1000, 1000, false, true), EscapeAction::StopTurn);                          // same instant
  // Stop wins over a queued send in the same thread.
  EXPECT_EQ(DecideEscape(1300, 1000, true, true), EscapeAction::StopTurn);
}

TEST(DecideEscapeTest, SecondPressAfterTheWindowIsASinglePress) {
  EXPECT_EQ(DecideEscape(1000 + kDoubleEscapeWindowMs + 1, 1000, false, true), EscapeAction::Armed);
  EXPECT_EQ(DecideEscape(1501, 1000, true, true), EscapeAction::CancelQueued);
}

TEST(DecideEscapeTest, SecondPressWithoutATurnHereFallsBack) {
  EXPECT_EQ(DecideEscape(1300, 1000, true, false), EscapeAction::CancelQueued);
  EXPECT_EQ(DecideEscape(1300, 1000, false, false), EscapeAction::None);
}

TEST(DecideEscapeTest, ClockGoingBackwardsIsNotADoublePress) {
  EXPECT_EQ(DecideEscape(900, 1000, false, true), EscapeAction::Armed);
  EXPECT_EQ(DecideEscape(900, 1000, true, true), EscapeAction::CancelQueued);
}

QueuedAgentSend Item(const std::string& thread, const std::string& text = "q") {
  QueuedAgentSend item;
  item.thread_id = thread;
  item.text = text;
  return item;
}

TEST(AgentSendQueueTest, StartsEmpty) {
  AgentSendQueue queue;
  EXPECT_TRUE(queue.Empty());
  EXPECT_EQ(queue.Size(), 0u);
  EXPECT_FALSE(queue.Has("A"));
  EXPECT_EQ(queue.Find("A"), nullptr);
  EXPECT_FALSE(queue.PopFront().has_value());
  EXPECT_FALSE(queue.Take("A").has_value());
}

TEST(AgentSendQueueTest, FirstInFirstOut) {
  AgentSendQueue queue;
  EXPECT_TRUE(queue.Enqueue(Item("A")));
  EXPECT_TRUE(queue.Enqueue(Item("B")));
  EXPECT_TRUE(queue.Enqueue(Item("C")));
  EXPECT_EQ(queue.Size(), 3u);
  EXPECT_FALSE(queue.Empty());
  EXPECT_EQ(queue.PopFront()->thread_id, "A");
  EXPECT_EQ(queue.PopFront()->thread_id, "B");
  EXPECT_EQ(queue.PopFront()->thread_id, "C");
  EXPECT_TRUE(queue.Empty());
}

TEST(AgentSendQueueTest, OneItemPerThread) {
  AgentSendQueue queue;
  EXPECT_TRUE(queue.Enqueue(Item("A", "first")));
  EXPECT_FALSE(queue.Enqueue(Item("A", "second")));
  EXPECT_EQ(queue.Size(), 1u);
  ASSERT_NE(queue.Find("A"), nullptr);
  EXPECT_EQ(queue.Find("A")->text, "first");
}

TEST(AgentSendQueueTest, TakeRemovesTheRightOneAndKeepsOrder) {
  AgentSendQueue queue;
  queue.Enqueue(Item("A"));
  queue.Enqueue(Item("B", "bee"));
  queue.Enqueue(Item("C"));
  const auto taken = queue.Take("B");
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->text, "bee");
  EXPECT_FALSE(queue.Has("B"));
  EXPECT_EQ(queue.Size(), 2u);
  EXPECT_EQ(queue.PopFront()->thread_id, "A");
  EXPECT_EQ(queue.PopFront()->thread_id, "C");
  EXPECT_FALSE(queue.Take("B").has_value()); // already gone
}

TEST(AgentSendQueueTest, PushFrontPutsItBackAtTheHead) {
  AgentSendQueue queue;
  queue.Enqueue(Item("B"));
  queue.Enqueue(Item("C"));
  queue.PushFront(Item("A"));
  EXPECT_EQ(queue.Size(), 3u);
  EXPECT_EQ(queue.PopFront()->thread_id, "A");
  EXPECT_EQ(queue.PopFront()->thread_id, "B");
  EXPECT_EQ(queue.PopFront()->thread_id, "C");
}

TEST(AgentSendQueueTest, PushFrontIsIgnoredWhenThatThreadAlreadyHasOne) {
  AgentSendQueue queue;
  queue.Enqueue(Item("B", "kept"));
  queue.Enqueue(Item("C"));
  queue.PushFront(Item("C", "late"));
  EXPECT_EQ(queue.Size(), 2u);
  EXPECT_EQ(queue.Find("C")->text, "q");
  EXPECT_EQ(queue.PopFront()->thread_id, "B");
}

} // namespace
} // namespace pbr
