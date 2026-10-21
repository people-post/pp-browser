#include "gui/chat/HeldAgentSend.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = std::chrono::steady_clock;

const Clock::time_point kT0 = Clock::time_point{} + std::chrono::hours(1);

TEST(HeldAgentSendTest, NothingHeldIsIdle) {
  HeldAgentSend slot;
  EXPECT_FALSE(slot.Holding());
  HeldAgentSend::Send out;
  EXPECT_EQ(slot.Poll(/*agent_ready=*/true, kT0, out), HeldAgentSend::Step::Idle);
}

// A tap while the assistant is still connecting waits; it goes out once the assistant is ready.
TEST(HeldAgentSendTest, SendGoesOutOnceTheAgentIsReady) {
  HeldAgentSend slot;
  slot.Hold({.text = "latest briefs", .payload = "{\"kind\":\"articles\"}", .from_home_chip = true, .thread_id = "t1"},
            kT0);
  ASSERT_TRUE(slot.Holding());

  HeldAgentSend::Send out;
  EXPECT_EQ(slot.Poll(false, kT0 + std::chrono::seconds(1), out), HeldAgentSend::Step::Wait);
  EXPECT_TRUE(slot.Holding());

  EXPECT_EQ(slot.Poll(true, kT0 + std::chrono::seconds(2), out), HeldAgentSend::Step::Send);
  EXPECT_EQ(out.text, "latest briefs");
  ASSERT_TRUE(out.payload.has_value());
  EXPECT_EQ(*out.payload, "{\"kind\":\"articles\"}");
  EXPECT_TRUE(out.from_home_chip);
  EXPECT_EQ(out.thread_id, "t1");
  EXPECT_FALSE(slot.Holding()); // sent once, never twice
  EXPECT_EQ(slot.Poll(true, kT0 + std::chrono::seconds(3), out), HeldAgentSend::Step::Idle);
}

// Only one send is held: a later tap replaces the earlier one and restarts the wait.
TEST(HeldAgentSendTest, LaterTapReplacesTheHeldOne) {
  HeldAgentSend slot;
  slot.Hold({.text = "first", .thread_id = "t1"}, kT0);
  slot.Hold({.text = "second", .thread_id = "t2"}, kT0 + std::chrono::seconds(10));

  HeldAgentSend::Send out;
  EXPECT_EQ(slot.Poll(false, kT0 + kHeldAgentSendTimeout, out), HeldAgentSend::Step::Wait);
  EXPECT_EQ(slot.Poll(true, kT0 + kHeldAgentSendTimeout + std::chrono::seconds(1), out), HeldAgentSend::Step::Send);
  EXPECT_EQ(out.text, "second");
  EXPECT_EQ(out.thread_id, "t2");
  EXPECT_FALSE(out.payload.has_value());
}

// The wait is bounded: if the assistant never gets ready the send is dropped once, with Expired.
TEST(HeldAgentSendTest, WaitIsBounded) {
  HeldAgentSend slot;
  slot.Hold({.text = "hi"}, kT0);

  HeldAgentSend::Send out;
  EXPECT_EQ(slot.Poll(false, kT0 + kHeldAgentSendTimeout - std::chrono::milliseconds(1), out),
            HeldAgentSend::Step::Wait);
  EXPECT_EQ(slot.Poll(false, kT0 + kHeldAgentSendTimeout, out), HeldAgentSend::Step::Expired);
  EXPECT_FALSE(slot.Holding());
  EXPECT_EQ(slot.Poll(true, kT0 + kHeldAgentSendTimeout + std::chrono::seconds(1), out), HeldAgentSend::Step::Idle);
}

TEST(HeldAgentSendTest, ClearDropsTheHeldSend) {
  HeldAgentSend slot;
  slot.Hold({.text = "hi"}, kT0);
  slot.Clear();
  EXPECT_FALSE(slot.Holding());
  HeldAgentSend::Send out;
  EXPECT_EQ(slot.Poll(true, kT0, out), HeldAgentSend::Step::Idle);
}

} // namespace
} // namespace pbr
