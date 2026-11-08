#include "foundation/platform/CallPushAge.h"

#include <gtest/gtest.h>

#include <limits>

namespace pbr {
namespace {

// P017 (#555 B187): a VoIP push iOS held on the phone for minutes rang as a ghost call. The relay's `sent_at` says how
// old it is; a push too old to ring is reported to CallKit (iOS requires it) and ended at once.

constexpr int64_t kNow = 1'762'600'000'000; // 2025-11, Unix ms

TEST(CallPushAgeTest, AFreshPushRings) {
  const CallPushAge age = JudgeCallPushAge(static_cast<double>(kNow - 3'000), kNow);
  EXPECT_TRUE(age.known);
  EXPECT_EQ(age.age_ms, 3'000);
  EXPECT_FALSE(age.stale);
  EXPECT_FALSE(JudgeCallPushAge(static_cast<double>(kNow - kCallPushStaleAfterMs), kNow).stale) << "the bound rings";
}

TEST(CallPushAgeTest, APushPastMostOfARingIsStale) {
  const CallPushAge age = JudgeCallPushAge(static_cast<double>(kNow - kCallPushStaleAfterMs - 1), kNow);
  EXPECT_TRUE(age.known);
  EXPECT_TRUE(age.stale);
  // The issue's push: the caller hung up four minutes before iOS handed it over.
  EXPECT_TRUE(JudgeCallPushAge(static_cast<double>(kNow - 4 * 60'000), kNow).stale);
}

TEST(CallPushAgeTest, NoSentAtRingsAsBefore) {
  const CallPushAge age = JudgeCallPushAge(std::nullopt, kNow); // an older relay
  EXPECT_FALSE(age.known);
  EXPECT_FALSE(age.stale);
}

TEST(CallPushAgeTest, APhoneClockBehindTheRelaysReadsFresh) {
  const CallPushAge age = JudgeCallPushAge(static_cast<double>(kNow + 90'000), kNow);
  EXPECT_TRUE(age.known);
  EXPECT_EQ(age.age_ms, 0) << "a negative age counts as fresh";
  EXPECT_FALSE(age.stale);
}

TEST(CallPushAgeTest, AnAgeOnlyAClockErrorExplainsRingsAsBefore) {
  // A phone clock hours ahead must not silence every call: the catch-up still ends a call that is over.
  const CallPushAge age = JudgeCallPushAge(static_cast<double>(kNow - 3 * 3600'000LL), kNow);
  EXPECT_TRUE(age.known);
  EXPECT_FALSE(age.stale);
}

TEST(CallPushAgeTest, GarbageSaysNothing) {
  for (const double garbage : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), -1.0, 0.0,
                               static_cast<double>(kNow / 1000) /* seconds, not ms */, 1e300}) {
    const CallPushAge age = JudgeCallPushAge(garbage, kNow);
    EXPECT_FALSE(age.known) << garbage;
    EXPECT_FALSE(age.stale) << garbage;
  }
}

} // namespace
} // namespace pbr
