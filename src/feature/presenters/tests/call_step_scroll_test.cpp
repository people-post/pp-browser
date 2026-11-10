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

// The incoming-call page on an iPhone SE-class page (647dp: 667 less a 20dp status bar, no bottom inset): the three
// lines give way to price / conflict / guest text, never the other way round.
TEST(CallStepScrollTest, TheRingKeepsItsStepsWhereTheyFitAndDropsThemFirstOnAShortPaidRing) {
  constexpr int kSe = 647;
  CallRingLayoutInput in;
  in.page_height_dp = kSe;
  EXPECT_TRUE(CallRingShowsSteps(in)) << "plain ring";
  in.conflict = true;
  EXPECT_TRUE(CallRingShowsSteps(in)) << "conflict ring";
  in.conflict = false;
  in.guest = true;
  EXPECT_TRUE(CallRingShowsSteps(in)) << "guest ring";
  in.guest = false;
  in.pricing = true;
  in.charge_enabled = true;
  EXPECT_FALSE(CallRingShowsSteps(in)) << "paid ring, charge enabled";
  in.charge_enabled = false;
  EXPECT_FALSE(CallRingShowsSteps(in)) << "paid ring, charge disabled with its hint";
  in.page_height_dp = 900;
  EXPECT_TRUE(CallRingShowsSteps(in)) << "a tall page keeps them";
  in.page_height_dp = 0;
  EXPECT_TRUE(CallRingShowsSteps(in)) << "page height not known: shown";
}

TEST(CallStepScrollTest, AGroupRingNeedsMoreRoomThanAOneToOneRing) {
  CallRingLayoutInput in;
  in.page_height_dp = 510;
  EXPECT_TRUE(CallRingShowsSteps(in));
  in.group = true;
  EXPECT_FALSE(CallRingShowsSteps(in));
}

} // namespace
} // namespace pbr
