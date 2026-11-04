#include "gui/chat/HeldAgentSend.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::hours(1);

TEST(HeldAgentSendTest, NothingHeldIsIdle) {
  HeldAgentSend held;
  EXPECT_FALSE(held.Holding());
  const auto polled = held.Poll(/*agent_ready=*/true, kT0);
  EXPECT_TRUE(polled.ready.empty());
  EXPECT_EQ(polled.expired, 0u);
}

// A tap while the assistant is still connecting waits; it goes out once the assistant is ready.
TEST(HeldAgentSendTest, SendGoesOutOnceTheAgentIsReady) {
  HeldAgentSend held;
  held.Hold({.text = "latest briefs", .payload = "{\"kind\":\"articles\"}", .from_home_chip = true, .thread_id = "t1"},
            kT0);
  ASSERT_TRUE(held.Holding("t1"));

  EXPECT_TRUE(held.Poll(false, kT0 + std::chrono::seconds(1)).ready.empty());
  EXPECT_TRUE(held.Holding());

  const auto polled = held.Poll(true, kT0 + std::chrono::seconds(2));
  ASSERT_EQ(polled.ready.size(), 1u);
  const HeldAgentSend::Send& out = polled.ready.front();
  EXPECT_EQ(out.text, "latest briefs");
  ASSERT_TRUE(out.payload.has_value());
  EXPECT_EQ(*out.payload, "{\"kind\":\"articles\"}");
  EXPECT_TRUE(out.from_home_chip);
  EXPECT_EQ(out.thread_id, "t1");
  EXPECT_FALSE(held.Holding()); // sent once, never twice
  EXPECT_TRUE(held.Poll(true, kT0 + std::chrono::seconds(3)).ready.empty());
}

// One send per thread: a later tap in the same thread replaces the earlier one and restarts its wait.
TEST(HeldAgentSendTest, LaterTapInTheSameThreadReplacesTheHeldOne) {
  HeldAgentSend held;
  held.Hold({.text = "first", .thread_id = "t1"}, kT0);
  held.Hold({.text = "second", .thread_id = "t1"}, kT0 + std::chrono::seconds(10));

  EXPECT_EQ(held.Poll(false, kT0 + kHeldAgentSendTimeout).expired, 0u);
  const auto polled = held.Poll(true, kT0 + kHeldAgentSendTimeout + std::chrono::seconds(1));
  ASSERT_EQ(polled.ready.size(), 1u);
  EXPECT_EQ(polled.ready.front().text, "second");
  EXPECT_FALSE(polled.ready.front().payload.has_value());
}

// Sends of different threads are held side by side and leave in the order they were made.
TEST(HeldAgentSendTest, EachThreadKeepsItsOwnSend) {
  HeldAgentSend held;
  held.Hold({.text = "a", .thread_id = "t1"}, kT0);
  held.Hold({.text = "b", .thread_id = "t2"}, kT0 + std::chrono::seconds(1));
  EXPECT_TRUE(held.Holding("t1"));
  EXPECT_TRUE(held.Holding("t2"));
  EXPECT_FALSE(held.Holding("t3"));

  const auto polled = held.Poll(true, kT0 + std::chrono::seconds(2));
  ASSERT_EQ(polled.ready.size(), 2u);
  EXPECT_EQ(polled.ready[0].thread_id, "t1");
  EXPECT_EQ(polled.ready[1].thread_id, "t2");
}

// The wait is bounded: if the assistant never gets ready each send is given up once, at its own deadline.
TEST(HeldAgentSendTest, WaitIsBounded) {
  HeldAgentSend held;
  held.Hold({.text = "hi", .thread_id = "t1"}, kT0);
  held.Hold({.text = "later", .thread_id = "t2"}, kT0 + std::chrono::seconds(5));

  EXPECT_EQ(held.Poll(false, kT0 + kHeldAgentSendTimeout - std::chrono::milliseconds(1)).expired, 0u);
  EXPECT_EQ(held.Poll(false, kT0 + kHeldAgentSendTimeout).expired, 1u);
  EXPECT_FALSE(held.Holding("t1"));
  EXPECT_TRUE(held.Holding("t2"));
  EXPECT_EQ(held.Poll(false, kT0 + kHeldAgentSendTimeout + std::chrono::seconds(5)).expired, 1u);
  EXPECT_FALSE(held.Holding());
  EXPECT_TRUE(held.Poll(true, kT0 + kHeldAgentSendTimeout + std::chrono::seconds(6)).ready.empty());
}

TEST(HeldAgentSendTest, ClearDropsTheHeldSends) {
  HeldAgentSend held;
  held.Hold({.text = "hi", .thread_id = "t1"}, kT0);
  held.Hold({.text = "ho", .thread_id = "t2"}, kT0);
  held.Clear();
  EXPECT_FALSE(held.Holding());
  EXPECT_TRUE(held.Poll(true, kT0).ready.empty());
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

// Once the assistant is ready the held send goes out only into the thread it was made in, and not over a reply
// still loading there. Otherwise typed text goes back into the composer; a chip is dropped (it can be re-tapped).
TEST(HeldAgentSendTest, FateWhenTheAssistantIsReady) {
  const HeldAgentSend::Send typed{.text = "hi", .thread_id = "t1"};
  const HeldAgentSend::Send chip{.text = "latest briefs", .from_home_chip = true, .thread_id = "t1"};

  EXPECT_EQ(HeldSendFateOf(typed, "t1", /*reply_loading=*/false), HeldSendFate::Send);
  EXPECT_EQ(HeldSendFateOf(chip, "t1", false), HeldSendFate::Send);

  EXPECT_EQ(HeldSendFateOf(typed, "t2", false), HeldSendFate::BackToDraft);
  EXPECT_EQ(HeldSendFateOf(typed, "t1", true), HeldSendFate::BackToDraft);
  EXPECT_EQ(HeldSendFateOf(chip, "t2", false), HeldSendFate::Drop);
  EXPECT_EQ(HeldSendFateOf(chip, "t1", true), HeldSendFate::Drop);
}

} // namespace
} // namespace pbr
