#include "lib/ui/MentionNotifyPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr int64_t kSec = 1000;

TEST(MentionNotifyPolicyTest, NotificationsOffNeverNotifies) {
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(false, false, false, std::nullopt, 0));
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(false, true, true, std::nullopt, 0));
}

TEST(MentionNotifyPolicyTest, UnmutedAlwaysNotifies) {
  EXPECT_TRUE(ShouldNotifyBackgroundMessage(true, false, false, std::nullopt, 0));
  EXPECT_TRUE(ShouldNotifyBackgroundMessage(true, false, true, 0, 1));
}

TEST(MentionNotifyPolicyTest, MutedWithoutMentionStaysSilent) {
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(true, true, false, std::nullopt, 100 * kSec));
}

TEST(MentionNotifyPolicyTest, MutedMentionNotifiesAtMostOncePerMinute) {
  std::optional<int64_t> last;
  const int64_t t0 = 1000 * kSec;
  auto step = [&](const int64_t now) {
    const bool notify = ShouldNotifyBackgroundMessage(true, true, true, last, now);
    if (notify) {
      last = now;
    }
    return notify;
  };
  EXPECT_TRUE(step(t0));
  EXPECT_FALSE(step(t0 + 30 * kSec));
  EXPECT_TRUE(step(t0 + 61 * kSec));
}

TEST(MentionNotifyPolicyTest, ExactlyAMinuteCounts) {
  EXPECT_TRUE(ShouldNotifyBackgroundMessage(true, true, true, 0, 60 * kSec));
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(true, true, true, 0, 60 * kSec - 1));
}

TEST(MentionNotifyPolicyTest, TwoThreadsAreIndependent) {
  std::optional<int64_t> last_a;
  std::optional<int64_t> last_b;
  EXPECT_TRUE(ShouldNotifyBackgroundMessage(true, true, true, last_a, 10 * kSec));
  last_a = 10 * kSec;
  // Thread B has never notified: A's recent notice does not hold it back.
  EXPECT_TRUE(ShouldNotifyBackgroundMessage(true, true, true, last_b, 20 * kSec));
  last_b = 20 * kSec;
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(true, true, true, last_a, 30 * kSec));
  EXPECT_FALSE(ShouldNotifyBackgroundMessage(true, true, true, last_b, 30 * kSec));
}

} // namespace
} // namespace pbr
