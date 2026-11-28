#include "gui/shell/SafeAreaSettle.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using std::chrono::milliseconds;

TEST(SafeAreaSettleTest, AFirstReadKeepsFramesComingForAWhileAndThenStops) {
  SafeAreaSettle settle;
  const SafeAreaSettle::Clock::time_point t0{};
  EXPECT_TRUE(settle.Observe(t0, 1180, 820, 0));
  EXPECT_TRUE(settle.Observe(t0 + milliseconds(2000), 1180, 820, 0));
  EXPECT_FALSE(settle.Observe(t0 + SafeAreaSettle::kSettle + milliseconds(1), 1180, 820, 0));
}

TEST(SafeAreaSettleTest, AnInsetThatArrivesLateStartsTheWaitAgain) {
  SafeAreaSettle settle;
  const SafeAreaSettle::Clock::time_point t0{};
  settle.Observe(t0, 1180, 820, 0);
  const auto late = t0 + SafeAreaSettle::kSettle + milliseconds(500);
  EXPECT_FALSE(settle.Observe(late, 1180, 820, 0));
  EXPECT_TRUE(settle.Observe(late, 1180, 820, 28)) << "the controls' inset appeared after the quiet period";
  EXPECT_TRUE(settle.Observe(late + milliseconds(2000), 1180, 820, 28));
  EXPECT_FALSE(settle.Observe(late + SafeAreaSettle::kSettle + milliseconds(1), 1180, 820, 28));
}

TEST(SafeAreaSettleTest, AResizedWindowStartsTheWaitAgain) {
  SafeAreaSettle settle;
  const SafeAreaSettle::Clock::time_point t0{};
  settle.Observe(t0, 1180, 820, 28);
  const auto later = t0 + SafeAreaSettle::kSettle + milliseconds(10);
  EXPECT_FALSE(settle.Observe(later, 1180, 820, 28));
  EXPECT_TRUE(settle.Observe(later, 900, 820, 28));
}

} // namespace
} // namespace pbr
