// Card asks (M021 d6 / M025): an envelope's unsigned `sender_card_at` never stops a later ask; asks are bounded.
#include "lib/messaging/CardAskGate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace pbr {
namespace {

using Source = CardAskGate::Source;
constexpr int64_t kNow = 1'800'000'000'000;

TEST(CardAskGateTest, AForgedHugeVersionDoesNotBlockAGenuineAsk) {
  CardAskGate gate;
  const int64_t forged = std::numeric_limits<int64_t>::max();
  EXPECT_TRUE(gate.Allow("bob", forged, Source::Unverified, kNow));
  EXPECT_TRUE(gate.Allow("bob", kNow - 1000, Source::Verified, kNow + 1))
      << "an unverified hint is no version: a verified newer list still asks at once";
}

TEST(CardAskGateTest, AVersionPastTheSkewCountsForNothingEvenVerified) {
  CardAskGate gate;
  EXPECT_TRUE(gate.Allow("bob", kNow + kCardAtClockSkewMs + 1, Source::Verified, kNow));
  EXPECT_TRUE(gate.Allow("bob", kNow - 1000, Source::Verified, kNow + 1))
      << "a hint beyond the skew recorded nothing to be newer than";
  EXPECT_FALSE(gate.Allow("bob", kNow - 1000, Source::Verified, kNow + 2)) << "the same list, asked just now";
}

TEST(CardAskGateTest, TheSameListIsAskedAgainOnlyAfterTheGap) {
  CardAskGate gate;
  EXPECT_TRUE(gate.Allow("bob", kNow - 5000, Source::Verified, kNow));
  EXPECT_FALSE(gate.Allow("bob", kNow - 5000, Source::Verified, kNow + kCardReaskGapMs - 1));
  EXPECT_TRUE(gate.Allow("bob", kNow - 5000, Source::Verified, kNow + kCardReaskGapMs))
      << "the answer may have been lost";
  EXPECT_TRUE(gate.Allow("bob", kNow, Source::Verified, kNow + kCardReaskGapMs + 1)) << "a newer list asks at once";
}

TEST(CardAskGateTest, AnUnverifiedAskIsOncePerAccountPerGap) {
  CardAskGate gate;
  EXPECT_TRUE(gate.Allow("bob", kNow, Source::Unverified, kNow));
  EXPECT_FALSE(gate.Allow("bob", kNow + 1, Source::Unverified, kNow + 1000));
  EXPECT_TRUE(gate.Allow("carol", kNow, Source::Unverified, kNow + 1000)) << "another account's on its own";
  EXPECT_TRUE(gate.Allow("bob", kNow, Source::Unverified, kNow + kCardReaskGapMs));
}

TEST(CardAskGateTest, UnverifiedAsksAreRateLimitedAcrossAccounts) {
  CardAskGate gate;
  for (size_t i = 0; i < kUnverifiedCardAsksPerWindow; ++i) {
    EXPECT_TRUE(gate.Allow("forged-" + std::to_string(i), kNow, Source::Unverified, kNow));
  }
  EXPECT_FALSE(gate.Allow("forged-next", kNow, Source::Unverified, kNow + 1));
  EXPECT_TRUE(gate.Allow("bob", kNow - 1000, Source::Verified, kNow + 1)) << "a verified ask is not in that limit";
  EXPECT_TRUE(gate.Allow("forged-next", kNow, Source::Unverified, kNow + kUnverifiedCardAskWindowMs))
      << "the window moved on";
}

TEST(CardAskGateTest, RemembersAtMostTheCapAndForgetsPastTheGap) {
  CardAskGate gate;
  for (size_t i = 0; i < kCardAskAccountsMax + 10; ++i) {
    EXPECT_TRUE(gate.Allow("acct-" + std::to_string(i), kNow, Source::Verified, kNow + static_cast<int64_t>(i)));
  }
  EXPECT_EQ(gate.Size(), kCardAskAccountsMax);
  EXPECT_TRUE(gate.Allow("acct-0", kNow, Source::Verified, kNow + 20)) << "the longest-asked went first";
  EXPECT_TRUE(gate.Allow("late", kNow, Source::Unverified, kNow + 10 * kCardReaskGapMs));
  EXPECT_EQ(gate.Size(), 1u) << "asks past the gap decide nothing and are forgotten";
}

} // namespace
} // namespace pbr
