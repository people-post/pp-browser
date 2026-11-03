// Phone page sheet (new group): swipe down to close, else spring back (Kenneth 2026-11-04).
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
