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
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0));
  EXPECT_TRUE(tap.TakeHintedRoute("m2", t0));
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
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0)) << "no tap, no route";
  tap.Record("", false, "m1", t0);
  EXPECT_FALSE(tap.TakeHintedRoute("m0", t0)) << "another message leaves the tap waiting";
  EXPECT_TRUE(tap.TakeHintedRoute("m1", t0 + PendingNotificationTap::kMessageRouteTtl));
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0)) << "once per tap";

  tap.Record("", false, "m1", t0);
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0 + PendingNotificationTap::kMessageRouteTtl + std::chrono::seconds(1)));
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0)) << "an expired tap is dropped";
}

TEST(PendingNotificationTapTest, AHintedRouteAlsoServesACallTapByItsCallId) {
  PendingNotificationTap tap;
  tap.Record("", true, "c1", t0);
  EXPECT_FALSE(tap.TakeHintedRoute("c0", t0));
  EXPECT_TRUE(tap.TakeHintedRoute("c1", t0));
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0)) << "one tap, one route";
}

TEST(PendingNotificationTapTest, AHintedRouteNeedsAHint) {
  PendingNotificationTap tap;
  tap.Record("", true, "", t0);
  EXPECT_FALSE(tap.TakeHintedRoute("", t0));
  EXPECT_FALSE(tap.TakeHintedRoute("m1", t0)) << "a hintless call tap is not any message's";
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0));
}

TEST(PendingNotificationTapTest, FreshHintDoesNotConsumeAndDropsAnExpiredTap) {
  PendingNotificationTap tap;
  EXPECT_EQ(tap.FreshHint(t0), "");
  tap.Record("", false, "m1", t0);
  EXPECT_EQ(tap.FreshHint(t0), "m1");
  EXPECT_EQ(tap.FreshHint(t0), "m1");
  EXPECT_TRUE(tap.TakeHintedRoute("m1", t0));
  EXPECT_EQ(tap.FreshHint(t0), "");

  tap.Record("", false, "m1", t0);
  EXPECT_EQ(tap.FreshHint(t0 + PendingNotificationTap::kMessageRouteTtl + std::chrono::seconds(1)), "");
  EXPECT_EQ(tap.FreshHint(t0), "") << "an expired tap is dropped";

  tap.Record("thread-a", false, "", t0);
  EXPECT_EQ(tap.FreshHint(t0), "") << "a banner tap has no hint";
}

TEST(PendingNotificationTapTest, ALookupThenTakeRoutesOnceWhenTheMessageSyncedBeforeTheTap) {
  PendingNotificationTap tap;
  tap.Record("", false, "m1", t0);
  const std::string hint = tap.FreshHint(t0);
  ASSERT_FALSE(hint.empty());
  EXPECT_TRUE(tap.TakeHintedRoute(hint, t0));
  EXPECT_EQ(tap.FreshHint(t0), "");
}

// Tap before the handler is wired, then wiring consumes it; the reverse order finds nothing left.
TEST(PendingNotificationTapTest, TapBeforeWiringIsConsumedAtWiring) {
  PendingNotificationTap tap;
  tap.Record("thread-a", false, "", t0);
  auto wire = [&]() { return tap.TakeThread(); };
  EXPECT_EQ(wire(), std::optional<std::string>("thread-a"));
  EXPECT_EQ(tap.TakeThread(), std::nullopt) << "the posted dispatch finds the slot already consumed";
}

TEST(PendingNotificationTapTest, CallTapSeqCountsCallTapsAndIsNeverReset) {
  PendingNotificationTap tap;
  EXPECT_EQ(tap.CallTapSeq(), 0u);
  tap.Record("thread-a", false, "", t0);
  tap.Record("", false, "m1", t0);
  EXPECT_EQ(tap.CallTapSeq(), 0u) << "only call-alert taps count";
  tap.Record("", true, "", t0);
  EXPECT_EQ(tap.CallTapSeq(), 1u);
  EXPECT_TRUE(tap.TakeCallRoute("c1", t0));
  EXPECT_EQ(tap.CallTapSeq(), 1u) << "taking the route keeps the count";
  tap.Record("", true, "", t0);
  EXPECT_FALSE(tap.PeekCallRoute(t0 + std::chrono::seconds(91)));
  EXPECT_FALSE(tap.TakeCallRoute("c1", t0 + std::chrono::seconds(91)));
  EXPECT_EQ(tap.CallTapSeq(), 2u) << "expiry keeps the count";
  tap.Record("thread-b", false, "", t0);
  EXPECT_EQ(tap.CallTapSeq(), 2u);
}

TEST(PendingNotificationTapTest, CallTapNotificationAgeIsKeptForTheLatestCallTap) {
  using std::chrono::milliseconds;
  PendingNotificationTap tap;
  EXPECT_EQ(tap.CallTapNotificationAge(), std::nullopt);
  tap.Record("", true, "", t0, milliseconds(1200));
  EXPECT_EQ(tap.CallTapNotificationAge(), std::optional<milliseconds>(1200));
  tap.Record("", true, "", t0);
  EXPECT_EQ(tap.CallTapNotificationAge(), std::nullopt) << "a call tap without an age is unknown, not the old one";
  tap.Record("", true, "", t0, milliseconds(0));
  EXPECT_EQ(tap.CallTapNotificationAge(), std::optional<milliseconds>(0));
  tap.Record("thread-a", false, "", t0, milliseconds(5));
  EXPECT_EQ(tap.CallTapNotificationAge(), std::optional<milliseconds>(0)) << "a message tap does not touch it";
}

TEST(PendingNotificationTapTest, CallTapElapsedCountsFromTheLatestCallTap) {
  using std::chrono::milliseconds;
  PendingNotificationTap tap;
  EXPECT_EQ(tap.CallTapElapsed(t0), std::nullopt);
  tap.Record("thread-a", false, "", t0);
  EXPECT_EQ(tap.CallTapElapsed(t0), std::nullopt) << "only call-alert taps count";
  tap.Record("", true, "", t0 + milliseconds(1000));
  EXPECT_EQ(tap.CallTapElapsed(t0 + milliseconds(3500)), std::optional<milliseconds>(2500));
  tap.Record("", true, "", t0 + milliseconds(5000));
  EXPECT_EQ(tap.CallTapElapsed(t0 + milliseconds(5100)), std::optional<milliseconds>(100)) << "the newer tap";
}

} // namespace
} // namespace pbr
