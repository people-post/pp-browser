#include "feature/presenters/CallMediaHintClock.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// One-way audio reads as a hand-off in progress, and says "retrying" only after 8 s (#170).

TEST(CallMediaHintClockTest, SaysConnectingFirstThenRetryingAfterEightSeconds) {
  CallMediaHintClock clock;
  clock.Update("c1", true, 1'000);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.connecting_voice");
  EXPECT_STREQ(clock.SendingOnlyKey(true), "call.quality.hint.connecting_video");
  clock.Update("c1", true, 9'000);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.connecting_voice") << "exactly 8 s is not over 8 s";
  clock.Update("c1", true, 9'001);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.retrying_voice");
  EXPECT_STREQ(clock.SendingOnlyKey(true), "call.quality.hint.retrying_video");
}

TEST(CallMediaHintClockTest, ARecoveredLineOrANewCallRestartsTheClock) {
  CallMediaHintClock clock;
  clock.Update("c1", true, 1);
  clock.Update("c1", true, 20'000);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.retrying_voice");
  clock.Update("c1", false, 21'000);
  clock.Update("c1", true, 22'000);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.connecting_voice");
  clock.Update("c1", true, 40'000);
  clock.Update("c2", true, 41'000);
  EXPECT_STREQ(clock.SendingOnlyKey(false), "call.quality.hint.connecting_voice");
}

TEST(CallMediaHintClockTest, OtherAsymmetriesKeepTheirKeys) {
  CallMediaHintClock clock;
  EXPECT_STREQ(clock.KeyFor(CallAudioAsymmetry::None, false), "");
  EXPECT_STREQ(clock.KeyFor(CallAudioAsymmetry::ReceivingOnly, false), "call.quality.hint.receiving_only");
}

} // namespace
} // namespace pbr
