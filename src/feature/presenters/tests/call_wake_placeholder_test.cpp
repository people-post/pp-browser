#include "feature/presenters/CallWakePlaceholder.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The incoming-call placeholder page: starts on a fresh call tap, ends on ring / timeout / expiry.

using Outcome = CallWakePlaceholder::Outcome;
using Input = CallWakePlaceholder::Input;
using Kind = CallScreen::Kind;

Input Tap(const uint64_t seq, const int64_t now_ms) {
  Input in;
  in.tap_seq = seq;
  in.tap_fresh = true;
  in.now_ms = now_ms;
  return in;
}

TEST(CallWakePlaceholderTest, StartsOnAFreshTapAndStaysUpWhileNothingElseHappens) {
  CallWakePlaceholder p;
  EXPECT_FALSE(p.Step(Input{}).active) << "no tap, nothing";
  auto v = p.Step(Tap(1, 1'000));
  EXPECT_TRUE(v.active);
  EXPECT_EQ(v.outcome, Outcome::None);
  EXPECT_TRUE(p.Step(Tap(1, 30'000)).active) << "not ready: no timer but the tap's freshness";
}

TEST(CallWakePlaceholderTest, ARealRingTakesOverAndReportsHowLongItWasShown) {
  CallWakePlaceholder p;
  p.Step(Tap(1, 1'000));
  Input in = Tap(1, 3'500);
  in.screen_kind = Kind::Ringing;
  auto v = p.Step(in);
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::Ring);
  EXPECT_EQ(v.shown_ms, 2'500);
  EXPECT_EQ(v.ready_ms, std::nullopt);
  EXPECT_EQ(p.Step(in).outcome, Outcome::None) << "reported once";
}

TEST(CallWakePlaceholderTest, ReadyForTenSecondsWithoutARingTimesOut) {
  CallWakePlaceholder p;
  p.Step(Tap(1, 1'000));
  Input in = Tap(1, 4'000);
  in.messaging_ready = true;
  EXPECT_TRUE(p.Step(in).active);
  in.now_ms = 4'000 + 9'900;
  EXPECT_TRUE(p.Step(in).active) << "9.9 s is not enough";
  in.now_ms = 4'000 + 10'000;
  auto v = p.Step(in);
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::Timeout);
  EXPECT_EQ(v.shown_ms, 13'000);
  EXPECT_EQ(v.ready_ms, std::optional<int64_t>(3'000));
}

TEST(CallWakePlaceholderTest, MessagingAlreadyReadyAtStartCountsTheTenSecondsFromStart) {
  CallWakePlaceholder p;
  Input in = Tap(1, 1'000);
  in.messaging_ready = true;
  EXPECT_TRUE(p.Step(in).active);
  in.now_ms = 10'999;
  EXPECT_TRUE(p.Step(in).active);
  in.now_ms = 11'000;
  auto v = p.Step(in);
  EXPECT_EQ(v.outcome, Outcome::Timeout);
  EXPECT_EQ(v.ready_ms, std::optional<int64_t>(0));
}

TEST(CallWakePlaceholderTest, NotReadyNeverTimesOutButEndsExpiredWhenTheTapGoesStale) {
  CallWakePlaceholder p;
  p.Step(Tap(1, 0));
  EXPECT_TRUE(p.Step(Tap(1, 29'000)).active);
  Input in = Tap(1, 30'500);
  in.tap_fresh = false;
  auto v = p.Step(in);
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::Expired);
  EXPECT_EQ(v.shown_ms, 30'500);
  EXPECT_EQ(v.ready_ms, std::nullopt);
}

TEST(CallWakePlaceholderTest, TheSameTapNeverRestartsAfterAnyEnding) {
  for (const Outcome end : {Outcome::Ring, Outcome::Expired, Outcome::Timeout}) {
    CallWakePlaceholder p;
    p.Step(Tap(1, 0));
    Input in = Tap(1, 20'000);
    in.messaging_ready = true;
    p.Step(in); // ready from here
    in.now_ms = 21'000;
    if (end == Outcome::Ring) {
      in.screen_kind = Kind::Ringing;
    } else if (end == Outcome::Expired) {
      in.tap_fresh = false;
    } else {
      in.now_ms = 30'000;
    }
    EXPECT_EQ(p.Step(in).outcome, end);
    EXPECT_FALSE(p.Step(Tap(1, 31'000)).active) << "ending " << static_cast<int>(end);
  }
}

TEST(CallWakePlaceholderTest, ANewTapAfterAnEndingStartsAgain) {
  CallWakePlaceholder p;
  p.Step(Tap(1, 0));
  Input in = Tap(1, 500);
  in.tap_fresh = false;
  p.Step(in);
  EXPECT_TRUE(p.Step(Tap(2, 5'000)).active);
}

TEST(CallWakePlaceholderTest, ALateRingAfterTimeoutDoesNotReShowOrReport) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.messaging_ready = true;
  p.Step(in);
  in.now_ms = 10'000;
  EXPECT_EQ(p.Step(in).outcome, Outcome::Timeout);
  in.now_ms = 12'000;
  in.screen_kind = Kind::Ringing;
  auto v = p.Step(in);
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::None);
  in.screen_kind = Kind::None;
  in.now_ms = 13'000;
  EXPECT_FALSE(p.Step(in).active) << "the ring ended, the same tap still does not come back";
}

TEST(CallWakePlaceholderTest, ANotificationOlderThanSixtySecondsNeverStarts) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.tap_notification_age_ms = 60'001;
  EXPECT_FALSE(p.Step(in).active);
  in.tap_notification_age_ms = 5;
  in.now_ms = 1'000;
  EXPECT_FALSE(p.Step(in).active) << "a tap that failed the age test is handled for good";
  EXPECT_TRUE(p.Step(Tap(2, 2'000)).active) << "a new tap with unknown age starts";
}

TEST(CallWakePlaceholderTest, ANotificationExactlySixtySecondsOldStarts) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.tap_notification_age_ms = 60'000;
  EXPECT_TRUE(p.Step(in).active);
}

TEST(CallWakePlaceholderTest, ATapWhileACallScreenIsAlreadyUpNeverShows) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.screen_kind = Kind::InCall;
  EXPECT_FALSE(p.Step(in).active);
  EXPECT_EQ(p.Step(in).outcome, Outcome::None);
  in.screen_kind = Kind::None;
  in.now_ms = 1'000;
  EXPECT_FALSE(p.Step(in).active) << "handled when it arrived";
}

TEST(CallWakePlaceholderTest, ANewerTapWhileShownWaitsForItsOwnCallFromNow) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.messaging_ready = true;
  p.Step(in);
  in = Tap(2, 8'000);
  in.messaging_ready = true;
  EXPECT_TRUE(p.Step(in).active);
  in.now_ms = 17'000;
  EXPECT_TRUE(p.Step(in).active) << "ten seconds count from the newer tap";
  in.now_ms = 18'000;
  auto v = p.Step(in);
  EXPECT_EQ(v.outcome, Outcome::Timeout);
  EXPECT_EQ(v.shown_ms, 10'000);
}

TEST(CallWakePlaceholderTest, ANotificationFortySecondsOldShownTwentyOneSecondsEnds) {
  CallWakePlaceholder p;
  Input in = Tap(1, 0);
  in.tap_notification_age_ms = 40'000;
  EXPECT_TRUE(p.Step(in).active);
  in.now_ms = 20'000;
  EXPECT_TRUE(p.Step(in).active) << "40 s + 20 s is still within the 60 s ring";
  in.now_ms = 21'000;
  auto v = p.Step(in);
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::Expired);
  EXPECT_EQ(v.shown_ms, 21'000);
}

TEST(CallWakePlaceholderTest, UnknownAgeShownSixtyOneSecondsEndsEvenWhenNeverReady) {
  CallWakePlaceholder p;
  EXPECT_TRUE(p.Step(Tap(1, 0)).active);
  EXPECT_TRUE(p.Step(Tap(1, 60'000)).active) << "exactly the ring length is not over";
  auto v = p.Step(Tap(1, 61'000));
  EXPECT_FALSE(v.active);
  EXPECT_EQ(v.outcome, Outcome::Expired);
}

TEST(CallWakePlaceholderTest, NotReadyStaysUpFor59Seconds) {
  CallWakePlaceholder p;
  p.Step(Tap(1, 0));
  EXPECT_TRUE(p.Step(Tap(1, 30'000)).active);
  EXPECT_TRUE(p.Step(Tap(1, 59'000)).active);
}

} // namespace
} // namespace pbr
