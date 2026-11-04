// Phone page sheet (new group): swipe down to close, else spring back (Kenneth 2026-11-04). The ask-AI panel's sheet
// also stops at half height (DEV/PLAN-ui-batch5-ask-ai.md 5a).
#include "gui/shell/PageSheetModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr float kHeightDp = 760.f;

TEST(PageSheetModelTest, AQuarterOfTheHeightCloses) {
  EXPECT_FALSE(PageSheetModel::ShouldDismiss(kHeightDp * 0.25f - 1.f, kHeightDp, 0.f));
  EXPECT_TRUE(PageSheetModel::ShouldDismiss(kHeightDp * 0.25f, kHeightDp, 0.f));
  EXPECT_TRUE(PageSheetModel::ShouldDismiss(kHeightDp * 0.6f, kHeightDp, 0.f));
}

TEST(PageSheetModelTest, AFlickClosesAShortDrag) {
  EXPECT_TRUE(PageSheetModel::ShouldDismiss(60.f, kHeightDp, 900.f));
  EXPECT_FALSE(PageSheetModel::ShouldDismiss(60.f, kHeightDp, 300.f)) << "slow and short springs back";
  EXPECT_FALSE(PageSheetModel::ShouldDismiss(10.f, kHeightDp, 2000.f)) << "a wobbly tap is no flick";
}

TEST(PageSheetModelTest, UpwardOrNoDragNeverCloses) {
  EXPECT_FALSE(PageSheetModel::ShouldDismiss(0.f, kHeightDp, 5000.f));
  EXPECT_FALSE(PageSheetModel::ShouldDismiss(-300.f, kHeightDp, 5000.f));
}

using Release = PageSheetModel::Release;
constexpr float kHalfDp = 420.f;

TEST(PageSheetModelTest, WithoutAHalfStopOnlyCloseOrStay) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, false, kHeightDp * 0.3f, kHeightDp, 0.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, false, 40.f, kHeightDp, 0.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, false, -400.f, kHeightDp, -3000.f), Release::Stay);
}

TEST(PageSheetModelTest, FromHalfAQuarterUpOrAFlickUpGoesFull) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, -kHalfDp * 0.25f, kHalfDp, 0.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, -40.f, kHalfDp, -900.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, -kHalfDp * 0.25f + 1.f, kHalfDp, -100.f), Release::Stay);
}

TEST(PageSheetModelTest, FromHalfAQuarterDownOrAFlickDownCloses) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, kHalfDp * 0.25f, kHalfDp, 0.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, 40.f, kHalfDp, 900.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, true, 30.f, kHalfDp, 100.f), Release::Stay);
}

TEST(PageSheetModelTest, FromFullADragDownGoesBackToHalfNotClosed) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, true, kHeightDp * 0.25f, kHeightDp, 0.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, true, kHeightDp * 0.9f, kHeightDp, 3000.f), Release::ToHalf)
      << "even a long fast drag stops at half first";
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, true, 40.f, kHeightDp, 100.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, true, -200.f, kHeightDp, -3000.f), Release::Stay)
      << "nothing above full";
}

TEST(PageSheetModelTest, TheKeyboardShowsFullAndItsGoingBackRestoresTheStop) {
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Half, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Half, false), SheetDetent::Half) << "keyboard down: back to half";
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Full, false), SheetDetent::Full);
}

TEST(PageSheetModelTest, VelocityComesFromTheLastMoves) {
  PageSheetModel::Velocity velocity;
  velocity.Add(0.0, 0.f);
  velocity.Add(200.0, 20.f);  // slow start, dropped from the window
  velocity.Add(260.0, 50.f);
  velocity.Add(300.0, 90.f);
  EXPECT_NEAR(velocity.AtRelease(310.0), (90.f - 20.f) * 1000.f / 100.f, 1.f);
}

TEST(PageSheetModelTest, ADragThatStoppedBeforeReleaseHasNoSpeed) {
  PageSheetModel::Velocity velocity;
  velocity.Add(0.0, 0.f);
  velocity.Add(50.0, 80.f);
  EXPECT_FLOAT_EQ(velocity.AtRelease(400.0), 0.f);
}

TEST(PageSheetModelTest, OneSampleHasNoSpeed) {
  PageSheetModel::Velocity velocity;
  velocity.Add(0.0, 10.f);
  EXPECT_FLOAT_EQ(velocity.AtRelease(5.0), 0.f);
}

} // namespace
} // namespace pbr
