#include "gui/chat/AgentBusyRules.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(DecideSendTest, MessagesThatDoNotUseTheAiAlwaysGoOut) {
  for (const TurnStart start : {TurnStart::Yes, TurnStart::Full, TurnStart::BusyHere}) {
    EXPECT_EQ(DecideSend(false, start, false, false), SendDecision::Send);
    EXPECT_EQ(DecideSend(false, start, true, false), SendDecision::Send);
  }
  // Even with a queued AI send waiting in the target thread.
  EXPECT_EQ(DecideSend(false, TurnStart::Full, false, true), SendDecision::Send);
}

TEST(DecideSendTest, AiSendTheSessionsCanStartGoesOut) {
  EXPECT_EQ(DecideSend(true, TurnStart::Yes, false, false), SendDecision::Send);
}

TEST(DecideSendTest, AiSendWhileTheSessionsAreFullIsQueued) {
  EXPECT_EQ(DecideSend(true, TurnStart::Full, false, false), SendDecision::Queue);
}

TEST(DecideSendTest, AiSendInARunningThreadIsBlocked) {
  EXPECT_EQ(DecideSend(true, TurnStart::BusyHere, false, false), SendDecision::BlockedHere);
  // The screen still shows a turn there (its end not heard yet), whatever the sessions say.
  EXPECT_EQ(DecideSend(true, TurnStart::Yes, true, false), SendDecision::BlockedHere);
  EXPECT_EQ(DecideSend(true, TurnStart::Full, true, false), SendDecision::BlockedHere);
}

TEST(DecideSendTest, TargetWithAQueuedSendIsAlreadyQueued) {
  for (const TurnStart start : {TurnStart::Yes, TurnStart::Full, TurnStart::BusyHere}) {
    EXPECT_EQ(DecideSend(true, start, false, true), SendDecision::AlreadyQueued);
  }
}

TEST(AgentBusyStateTest, SeveralThreadsAtOnce) {
  AgentBusyState busy;
  EXPECT_FALSE(busy.Any());
  EXPECT_FALSE(busy.BusyIn("A"));
  busy.threads = {"A", "B"};
  EXPECT_TRUE(busy.Any());
  EXPECT_TRUE(busy.BusyIn("A"));
  EXPECT_TRUE(busy.BusyIn("B"));
  EXPECT_FALSE(busy.BusyIn("C"));
  EXPECT_FALSE(busy.BusyIn(""));
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

TEST(ChatPageShowsTest, OnlyTheOpenThreadWithItsPageOnScreen) {
  ShellChromeSnapshot compact;
  compact.layout_mode = LayoutMode::Compact;
  compact.nav_tab = NavTab::Sessions;
  compact.compact_chat_open = true;
  EXPECT_TRUE(ChatPageShows(compact, "A", "A"));
  EXPECT_FALSE(ChatPageShows(compact, "A", "B")) << "another chat";
  compact.compact_chat_open = false; // back on the list
  EXPECT_FALSE(ChatPageShows(compact, "A", "A"));

  ShellChromeSnapshot wide;
  wide.layout_mode = LayoutMode::Expanded;
  wide.nav_tab = NavTab::Sessions;
  wide.primary_pane_key = "chat";
  EXPECT_TRUE(ChatPageShows(wide, "A", "A"));
  wide.nav_tab = NavTab::Home;
  EXPECT_FALSE(ChatPageShows(wide, "A", "A")) << "Home shows its own composer";
  wide.nav_tab = NavTab::Contacts;
  wide.primary_pane_key = "contacts";
  EXPECT_FALSE(ChatPageShows(wide, "A", "A"));
  EXPECT_FALSE(ChatPageShows(wide, "", ""));
}

} // namespace
} // namespace pbr
