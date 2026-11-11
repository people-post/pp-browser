// Five quick taps on the version row toggle the advanced flag (DEV/PLAN-settings-reorg.md "连点 5 次的唯一定义").
#include "feature/settings/VersionTapCounter.h"

#include <gtest/gtest.h>

namespace pbr {

using Action = VersionTapCounter::Action;

TEST(VersionTapCounterTest, FiveQuickTapsToggleAndTheThirdAndFourthSayHowManyAreLeft) {
  VersionTapCounter counter;
  EXPECT_EQ(counter.Tap(0).action, Action::None);
  EXPECT_EQ(counter.Tap(300).action, Action::None);
  const auto third = counter.Tap(600);
  EXPECT_EQ(third.action, Action::Hint);
  EXPECT_EQ(third.taps_left, 2);
  const auto fourth = counter.Tap(900);
  EXPECT_EQ(fourth.action, Action::Hint);
  EXPECT_EQ(fourth.taps_left, 1);
  EXPECT_EQ(counter.Tap(1200).action, Action::Toggle);
}

TEST(VersionTapCounterTest, AGapOverTwoSecondsStartsAgain) {
  VersionTapCounter counter;
  counter.Tap(0);
  counter.Tap(100);
  counter.Tap(200); // hint: 2 left
  EXPECT_EQ(counter.Tap(200 + 2001).action, Action::None) << "more than two seconds: this is tap 1";
  EXPECT_EQ(counter.Tap(2400).action, Action::None);
  const auto third = counter.Tap(2500);
  EXPECT_EQ(third.action, Action::Hint);
  EXPECT_EQ(third.taps_left, 2);
}

TEST(VersionTapCounterTest, ExactlyTwoSecondsKeepsTheCount) {
  VersionTapCounter counter;
  counter.Tap(0);
  counter.Tap(2000);
  counter.Tap(4000);
  EXPECT_EQ(counter.Tap(6000).taps_left, 1) << "each gap is 2000 ms: the fourth tap";
  EXPECT_EQ(counter.Tap(8000).action, Action::Toggle);
}

TEST(VersionTapCounterTest, FiveMoreTapsAfterAToggleToggleAgain) {
  VersionTapCounter counter;
  uint64_t now = 0;
  for (int round = 0; round < 3; ++round) {
    for (int tap = 1; tap <= 4; ++tap) {
      EXPECT_NE(counter.Tap(now).action, Action::Toggle) << "round " << round << " tap " << tap;
      now += 500;
    }
    EXPECT_EQ(counter.Tap(now).action, Action::Toggle) << "round " << round;
    now += 500;
  }
}

} // namespace pbr
