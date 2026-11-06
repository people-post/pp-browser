#include "foundation/platform/PendingNotificationTap.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = PendingNotificationTap::Clock;
const Clock::time_point t0{};

TEST(PendingNotificationTapTest, AMessageTapWaitsAndIsTakenOnce) {
  PendingNotificationTap tap;
  EXPECT_EQ(tap.TakeThread(), std::nullopt);
  tap.Record("thread-a", false, "", t0);
  EXPECT_EQ(tap.TakeThread(), std::optional<std::string>("thread-a"));
  EXPECT_EQ(tap.TakeThread(), std::nullopt);
}

TEST(PendingNotificationTapTest, OnlyTheNewestTapIsKept) {
  PendingNotificationTap tap;
  tap.Record("thread-a", false, "", t0);
  tap.Record("thread-b", false, "", t0);
  EXPECT_EQ(tap.TakeThread(), std::optional<std::string>("thread-b"));

  tap.Record("", true, "", t0);
  tap.Record("thread-c", false, "", t0);
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0)) << "a later message tap cancels the call route";

  tap.Record("thread-d", false, "", t0);
  tap.Record("", true, "", t0);
  EXPECT_EQ(tap.TakeThread(), std::nullopt) << "a later call tap cancels the chat to open";
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0));

  tap.Record("", false, "m1", t0);
  tap.Record("", false, "m2", t0);
  EXPECT_FALSE(tap.TakeMessageRoute("m1", t0));
  EXPECT_TRUE(tap.TakeMessageRoute("m2", t0));
}

TEST(PendingNotificationTapTest, ACallRouteWithoutAHintTakesTheFirstRingOnce) {
  PendingNotificationTap tap;
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0)) << "no tap, no route";
  tap.Record("", true, "", t0);
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0 + std::chrono::seconds(5)));
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0 + std::chrono::seconds(6)));
}

TEST(PendingNotificationTapTest, ACallRouteWithAHintWaitsForItsOwnRing) {
  PendingNotificationTap tap;
  tap.Record("", true, "c2", t0);
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0)) << "another ring is not the tapped call";
  EXPECT_TRUE(tap.TakeCallRoute("c2", t0 + std::chrono::seconds(5))) << "the tap is still waiting";
  EXPECT_FALSE(tap.TakeCallRoute("c2", t0 + std::chrono::seconds(6)));
}

TEST(PendingNotificationTapTest, ACallRouteAtTheLimitStillCountsAndAfterItExpires) {
  PendingNotificationTap at_limit;
  at_limit.Record("", true, "c1", t0);
  EXPECT_TRUE(at_limit.TakeCallRoute("c1", t0 + PendingNotificationTap::kCallRouteTtl));

  PendingNotificationTap tap;
  tap.Record("", true, "c1", t0);
  const auto late = t0 + PendingNotificationTap::kCallRouteTtl + std::chrono::seconds(1);
  EXPECT_FALSE(tap.PeekCallRoute(late));
  EXPECT_FALSE(tap.TakeCallRoute("c1", late));
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0)) << "an expired tap does not come back";
}

TEST(PendingNotificationTapTest, PeekDoesNotConsume) {
  PendingNotificationTap tap;
  EXPECT_FALSE(tap.PeekCallRoute(t0));
  tap.Record("", true, "c1", t0);
  EXPECT_TRUE(tap.PeekCallRoute(t0));
  EXPECT_TRUE(tap.PeekCallRoute(t0));
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0));
  EXPECT_FALSE(tap.PeekCallRoute(t0));

  tap.Record("", false, "m1", t0);
  EXPECT_FALSE(tap.PeekCallRoute(t0)) << "a message tap is not a call hint for the PIN screen";
}

TEST(PendingNotificationTapTest, AMessageRouteMatchesItsMessageOnceAndExpires) {
  PendingNotificationTap tap;
  EXPECT_FALSE(tap.TakeMessageRoute("m1", t0)) << "no tap, no route";
  tap.Record("", false, "m1", t0);
  EXPECT_FALSE(tap.TakeMessageRoute("m0", t0)) << "another message leaves the tap waiting";
  EXPECT_TRUE(tap.TakeMessageRoute("m1", t0 + PendingNotificationTap::kMessageRouteTtl));
  EXPECT_FALSE(tap.TakeMessageRoute("m1", t0)) << "once per tap";

  tap.Record("", false, "m1", t0);
  EXPECT_FALSE(tap.TakeMessageRoute("m1", t0 + PendingNotificationTap::kMessageRouteTtl + std::chrono::seconds(1)));
  EXPECT_FALSE(tap.TakeMessageRoute("m1", t0)) << "an expired tap is dropped";
}

TEST(PendingNotificationTapTest, ACallTapIsNotAMessageRoute) {
  PendingNotificationTap tap;
  tap.Record("", true, "c1", t0);
  EXPECT_FALSE(tap.TakeMessageRoute("c1", t0));
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0));
}

// Tap before the handler is wired, then wiring consumes it; the reverse order finds nothing left.
TEST(PendingNotificationTapTest, TapBeforeWiringIsConsumedAtWiring) {
  PendingNotificationTap tap;
  tap.Record("thread-a", false, "", t0);
  auto wire = [&]() { return tap.TakeThread(); };
  EXPECT_EQ(wire(), std::optional<std::string>("thread-a"));
  EXPECT_EQ(tap.TakeThread(), std::nullopt) << "the posted dispatch finds the slot already consumed";
}

} // namespace
} // namespace pbr
