// Phone page sheets (Kenneth 2026-11-04): every sheet opens at about 4/5 of the screen and stops at full, 4/5, half
// and a third. Like Apple Maps a release goes to the nearest stop, a flick one stop on in its direction; it closes
// only let go below 85% of the screen or flicked down from the lowest stop. Kenneth: "我好像拉到3分之1处，它后就会
// 关掉了。是不是还可以再往下拉一点？"
#include "gui/shell/PageSheetModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Release = PageSheetModel::Release;

constexpr float kLayerDp = 800.f;
// The tops at this layer height: full 10, large 160, half 360, small 528; let go below 680 it closes.
constexpr float kFullTop = PageSheetModel::kFullTopDp;
constexpr float kLargeTop = kLayerDp * PageSheetModel::kLargeTopRatio;
constexpr float kHalfTop = kLayerDp * PageSheetModel::kHalfTopRatio;
constexpr float kSmallTop = kLayerDp * PageSheetModel::kSmallTopRatio;
constexpr float kCloseTop = kLayerDp * PageSheetModel::kDismissTopRatio;

TEST(PageSheetModelTest, TheStopsAreFullLargeHalfAndSmallFromTheTop) {
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Full, kLayerDp), 10.f);
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Large, kLayerDp), 160.f) << "about 4/5 of the screen";
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Half, kLayerDp), 360.f);
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Small, kLayerDp), 528.f) << "about a third of the screen";
  EXPECT_FLOAT_EQ(kCloseTop, 680.f);
}

TEST(PageSheetModelTest, ANewSheetStandsAtLarge) {
  EXPECT_EQ(OverlayEntry{}.sheet_detent, SheetDetent::Large);
}

TEST(PageSheetModelTest, ASlowReleaseGoesToTheNearestStop) {
  // From large: short of the half-way point it springs back, past it the next stop.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 90.f, kLayerDp, 0.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 110.f, kLayerDp, 0.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -70.f, kLayerDp, 0.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -80.f, kLayerDp, 0.f), Release::ToFull);
  // From full down: large, half or small, whichever the top is nearest.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 120.f, kLayerDp, 0.f), Release::ToLarge);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 400.f - kFullTop, kLayerDp, 0.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 500.f - kFullTop, kLayerDp, 0.f), Release::ToSmall);
  // From small up: half, large or full.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, -150.f, kLayerDp, 0.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, -(kSmallTop - 180.f), kLayerDp, 0.f), Release::ToLarge);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, -(kSmallTop - 40.f), kLayerDp, 0.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, -200.f, kLayerDp, 0.f), Release::Stay) << "nothing above full";
}

TEST(PageSheetModelTest, DraggedFromLargeToAThirdOfTheScreenItStopsAtSmallInsteadOfClosing) {
  // Kenneth's case: from 4/5 down until about a third of the screen still shows.
  const float third_top = kLayerDp * 2.f / 3.f;
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, third_top - kLargeTop, kLayerDp, 0.f), Release::ToSmall);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, third_top - kHalfTop, kLayerDp, 0.f), Release::ToSmall);
}

TEST(PageSheetModelTest, ItClosesOnlyLetGoBelowEightyFivePercent) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, kCloseTop - kSmallTop - 1.f, kLayerDp, 0.f), Release::Stay)
      << "just above the line: back to small";
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, kCloseTop - kSmallTop + 1.f, kLayerDp, 0.f),
            Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, kCloseTop - kLargeTop + 10.f, kLayerDp, 0.f),
            Release::Dismiss)
      << "a long slow drag from 4/5 to the bottom closes too";
}

TEST(PageSheetModelTest, AFlickMovesOneStopInItsDirection) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 30.f, kLayerDp, 900.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, 30.f, kLayerDp, 900.f), Release::ToSmall);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 30.f, kLayerDp, 3000.f), Release::ToLarge);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -30.f, kLayerDp, -900.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, -30.f, kLayerDp, -900.f), Release::ToHalf);
  // One stop on from where the top is, not from where the drag began.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, (kHalfTop - kLargeTop) + 20.f, kLayerDp, 3000.f),
            Release::ToSmall);
  // The flick's direction counts, not the drag's: dragged down, flicked back up.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 150.f, kLayerDp, -900.f), Release::Stay);
}

TEST(PageSheetModelTest, AFlickDownFromSmallCloses) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, 30.f, kLayerDp, 900.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, 30.f, kLayerDp, 100.f), Release::Stay) << "too slow";
  // Flicked down from higher up and already past small: it stops at small, unless let go below the closing line.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, kSmallTop - kHalfTop + 60.f, kLayerDp, 1500.f),
            Release::ToSmall);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, kCloseTop - kHalfTop + 10.f, kLayerDp, 1500.f),
            Release::Dismiss);
}

TEST(PageSheetModelTest, ATapThatWobblesIsNoFlick) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Small, 10.f, kLayerDp, 2000.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -10.f, kLayerDp, -2000.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 0.f, kLayerDp, 5000.f), Release::Stay);
}

TEST(PageSheetModelTest, TheKeyboardShowsFullAndItsGoingBackRestoresTheStop) {
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Large, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Half, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Small, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Large, false), SheetDetent::Large) << "keyboard down: back to 4/5";
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Half, false), SheetDetent::Half) << "keyboard down: back to half";
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Small, false), SheetDetent::Small) << "keyboard down: back to small";
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Full, false), SheetDetent::Full);
}

TEST(PageSheetModelTest, VelocityComesFromTheLastMoves) {
  PageSheetModel::Velocity velocity;
  velocity.Add(0.0, 0.f);
  velocity.Add(200.0, 20.f); // slow start, dropped from the window
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
