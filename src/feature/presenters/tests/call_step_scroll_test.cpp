#include "feature/presenters/CallStepScroll.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr int kRow = call_page_dp::kStepRow;

TEST(CallStepScrollTest, NothingMovesWhileTheReachedRowFits) {
  EXPECT_EQ(CallStepScrollRows(3, 5 * kRow), 0);
  EXPECT_EQ(CallStepScrollRows(4, 5 * kRow), 0);
  EXPECT_EQ(CallStepScrollRows(5, 5 * kRow), 0);
}

TEST(CallStepScrollTest, TheListFollowsTheReachedRowOneRowAtATime) {
  EXPECT_EQ(CallStepScrollRows(6, 5 * kRow), 1);
  EXPECT_EQ(CallStepScrollRows(7, 5 * kRow), 2);
}

TEST(CallStepScrollTest, ARowThatIsOnlyPartlyVisibleDoesNotCount) {
  EXPECT_EQ(CallStepScrollRows(7, 5 * kRow + kRow - 1), 2);
}

TEST(CallStepScrollTest, AnUnknownOrTooSmallAreaMovesNothing) {
  EXPECT_EQ(CallStepScrollRows(7, 0), 0);
  EXPECT_EQ(CallStepScrollRows(7, -40), 0);
  EXPECT_EQ(CallStepScrollRows(7, kRow - 1), 0);
}

TEST(CallStepScrollTest, TheAreaIsThePageLessWhatIsAboveAndBelowTheSteps) {
  using namespace call_page_dp;
  EXPECT_EQ(CallStepAreaHeightDp(kStepsStart + kControlsReserve + 170), 170);
  EXPECT_LT(CallStepAreaHeightDp(0), 0) << "a page height not known yet gives no area";
}

} // namespace
} // namespace pbr
