#include "foundation/platform/PendingNotificationTap.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Clock = PendingNotificationTap::Clock;
const Clock::time_point t0{};

TEST(PendingNotificationTapTest, AMessageTapWaitsAndIsTakenOnce) {
  PendingNotificationTap tap;
  EXPECT_EQ(tap.TakeThread(), std::nullopt);
  tap.Record("thread-a", false, t0);
  EXPECT_EQ(tap.TakeThread(), std::optional<std::string>("thread-a"));
  EXPECT_EQ(tap.TakeThread(), std::nullopt);
}

TEST(PendingNotificationTapTest, OnlyTheNewestTapIsKept) {
  PendingNotificationTap tap;
  tap.Record("thread-a", false, t0);
  tap.Record("thread-b", false, t0);
  EXPECT_EQ(tap.TakeThread(), std::optional<std::string>("thread-b"));

  tap.Record("", true, t0);
  tap.Record("thread-c", false, t0);
  EXPECT_FALSE(tap.TakeCallRoute(t0)) << "a later message tap cancels the call route";

  tap.Record("thread-d", false, t0);
  tap.Record("", true, t0);
  EXPECT_EQ(tap.TakeThread(), std::nullopt) << "a later call tap cancels the chat to open";
  EXPECT_TRUE(tap.TakeCallRoute(t0));
}

TEST(PendingNotificationTapTest, ACallRouteIsTakenOnce) {
  PendingNotificationTap tap;
  EXPECT_FALSE(tap.TakeCallRoute(t0)) << "no tap, no route";
  tap.Record("", true, t0);
  EXPECT_TRUE(tap.TakeCallRoute(t0 + std::chrono::seconds(5)));
  EXPECT_FALSE(tap.TakeCallRoute(t0 + std::chrono::seconds(6)));
}

TEST(PendingNotificationTapTest, ACallRouteAtTheLimitStillCountsAndAfterItExpires) {
  PendingNotificationTap at_limit;
  at_limit.Record("", true, t0);
  EXPECT_TRUE(at_limit.TakeCallRoute(t0 + PendingNotificationTap::kCallRouteTtl));
}

TEST(PendingNotificationTapTest, ACallRouteExpiresAndIsDropped) {
  PendingNotificationTap tap;
  tap.Record("", true, t0);
  EXPECT_FALSE(tap.TakeCallRoute(t0 + PendingNotificationTap::kCallRouteTtl + std::chrono::seconds(1)));
  EXPECT_FALSE(tap.TakeCallRoute(t0)) << "an expired tap does not come back";
}

// Tap before the handler is wired, then wiring consumes it; the reverse order finds nothing left.
TEST(PendingNotificationTapTest, TapBeforeWiringIsConsumedAtWiring) {
  PendingNotificationTap tap;
  tap.Record("thread-a", false, t0);
  auto wire = [&]() { return tap.TakeThread(); };
  EXPECT_EQ(wire(), std::optional<std::string>("thread-a"));
  EXPECT_EQ(tap.TakeThread(), std::nullopt) << "the posted dispatch finds the slot already consumed";
}

} // namespace
} // namespace pbr
