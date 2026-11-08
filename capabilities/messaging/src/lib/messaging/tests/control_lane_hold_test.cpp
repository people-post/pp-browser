// Lane messages that wait for their sender's card (#555): bounded per sender, in all and in time, oldest first.
#include "lib/messaging/ControlLaneHold.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

RelayEnvelope Message(const std::string& id) {
  RelayEnvelope envelope;
  envelope.message_id = id;
  return envelope;
}

TEST(ControlLaneHoldTest, ASendersMessagesComeBackOldestFirstAndOnce) {
  ControlLaneHold hold;
  EXPECT_TRUE(hold.Hold("bob", Message("b2"), 200, 200));
  EXPECT_TRUE(hold.Hold("carol", Message("c1"), 150, 200));
  EXPECT_TRUE(hold.Hold("bob", Message("b1"), 100, 200)) << "a replayed one keeps its first hold time";
  EXPECT_TRUE(hold.Holds("bob"));
  const auto taken = hold.Take("bob", 300);
  ASSERT_EQ(taken.size(), 2u);
  EXPECT_EQ(taken[0].envelope.message_id, "b1");
  EXPECT_EQ(taken[0].held_since_ms, 100);
  EXPECT_EQ(taken[1].envelope.message_id, "b2");
  EXPECT_FALSE(hold.Holds("bob"));
  EXPECT_TRUE(hold.Take("bob", 300).empty());
  EXPECT_EQ(hold.SizeOf("carol"), 1u) << "another sender's wait on";
}

TEST(ControlLaneHoldTest, ACopyThatCameBothWaysIsHeldOnce) {
  ControlLaneHold hold;
  EXPECT_TRUE(hold.Hold("bob", Message("m"), 100, 100));
  EXPECT_TRUE(hold.Hold("bob", Message("m"), 110, 110)) << "the relay's and the link's copy: it waits, once";
  EXPECT_EQ(hold.SizeOf("bob"), 1u);
  EXPECT_TRUE(hold.Hold("carol", Message("m"), 110, 110)) << "another sender's id is its own";
  EXPECT_EQ(hold.Size(), 2u);
  EXPECT_EQ(hold.Take("bob", 120).front().held_since_ms, 100) << "the first copy's wait";
}

TEST(ControlLaneHoldTest, AMessageWaitsNoLongerThanTheMaximum) {
  ControlLaneHold hold;
  ASSERT_TRUE(hold.Hold("bob", Message("old"), 0, 0));
  ASSERT_TRUE(hold.Hold("bob", Message("new"), 1000, 1000));
  ControlLaneHoldDrops drops;
  const auto taken = hold.Take("bob", kControlLaneHoldMaxMs + 500, &drops);
  ASSERT_EQ(taken.size(), 1u);
  EXPECT_EQ(taken[0].envelope.message_id, "new");
  EXPECT_EQ(drops.expired, 1u);
  EXPECT_FALSE(hold.Hold("bob", Message("again"), 0, kControlLaneHoldMaxMs + 1)) << "its time is up already";
}

TEST(ControlLaneHoldTest, OneSendersOldestGoesPastItsShare) {
  ControlLaneHold hold;
  for (size_t i = 0; i < kControlLaneHoldPerSender; ++i) {
    ASSERT_TRUE(hold.Hold("bob", Message("b" + std::to_string(i)), static_cast<int64_t>(i), 100));
  }
  ASSERT_TRUE(hold.Hold("carol", Message("c"), 50, 100));
  ControlLaneHoldDrops drops;
  ASSERT_TRUE(hold.Hold("bob", Message("newest"), 99, 100, &drops));
  EXPECT_EQ(drops.over_sender, 1u);
  EXPECT_EQ(hold.SizeOf("bob"), kControlLaneHoldPerSender);
  EXPECT_EQ(hold.SizeOf("carol"), 1u) << "a busy sender does not push out another's";
  const auto taken = hold.Take("bob", 100);
  EXPECT_EQ(taken.front().envelope.message_id, "b1") << "b0, the oldest, went";
  EXPECT_EQ(taken.back().envelope.message_id, "newest");
}

TEST(ControlLaneHoldTest, TheOldestOfAllGoesPastTheTotal) {
  ControlLaneHold hold;
  for (size_t i = 0; i < kControlLaneHoldTotal; ++i) {
    ASSERT_TRUE(hold.Hold("sender" + std::to_string(i), Message("m"), static_cast<int64_t>(i), 1000));
  }
  ControlLaneHoldDrops drops;
  ASSERT_TRUE(hold.Hold("late", Message("m"), 999, 1000, &drops));
  EXPECT_EQ(drops.over_total, 1u);
  EXPECT_EQ(hold.Size(), kControlLaneHoldTotal);
  EXPECT_FALSE(hold.Holds("sender0")) << "the longest waiting";
  EXPECT_TRUE(hold.Holds("sender1"));
  EXPECT_TRUE(hold.Holds("late"));
}

TEST(ControlLaneHoldTest, TheWaitCountsAsTimeOnTheWay) {
  RelayEnvelope relayed = Message("r");
  relayed.relay_created_at_ms = 5000;
  relayed.relay_server_time_ms = 6000;
  const RelayEnvelope aged = AgedByHold(relayed, 100, 400);
  EXPECT_EQ(aged.relay_created_at_ms, std::optional<int64_t>(5000));
  EXPECT_EQ(aged.relay_server_time_ms, std::optional<int64_t>(6300)) << "1 s at the relay, 300 ms held";

  const RelayEnvelope linked = AgedByHold(Message("l"), 100, 400);
  ASSERT_TRUE(linked.relay_created_at_ms && linked.relay_server_time_ms);
  EXPECT_EQ(*linked.relay_server_time_ms - *linked.relay_created_at_ms, 300) << "through a link: the wait alone";
}

} // namespace
} // namespace pbr
