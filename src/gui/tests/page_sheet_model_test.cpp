// Phone page sheets (Kenneth 2026-11-04): every sheet opens at about 4/5 of the screen and stops at half, 4/5 and
// full; a quarter of the way to the next stop or a flick moves it there, below half it closes.
#include "gui/shell/PageSheetModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Release = PageSheetModel::Release;

constexpr float kLayerDp = 800.f;
// The tops at this layer height: full 10, large 160, half 360; the sheet at half is 440 tall.
constexpr float kFullTop = PageSheetModel::kFullTopDp;
constexpr float kLargeTop = kLayerDp * PageSheetModel::kLargeTopRatio;
constexpr float kHalfTop = kLayerDp * PageSheetModel::kHalfTopRatio;

TEST(PageSheetModelTest, TheStopsAreFullLargeAndHalfFromTheTop) {
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Full, kLayerDp), 10.f);
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Large, kLayerDp), 160.f) << "about 4/5 of the screen";
  EXPECT_FLOAT_EQ(PageSheetModel::TopDp(SheetDetent::Half, kLayerDp), 360.f);
}

TEST(PageSheetModelTest, ANewSheetStandsAtLarge) {
  EXPECT_EQ(OverlayEntry{}.sheet_detent, SheetDetent::Large);
}

TEST(PageSheetModelTest, FromLargeAQuarterUpOrAFlickUpGoesFull) {
  const float way = kLargeTop - kFullTop;
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -way * 0.25f, kLayerDp, 0.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -30.f, kLayerDp, -900.f), Release::ToFull);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -way * 0.25f + 1.f, kLayerDp, -100.f), Release::Stay);
}

TEST(PageSheetModelTest, FromLargeAQuarterDownOrAFlickDownGoesHalf) {
  const float way = kHalfTop - kLargeTop;
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, way * 0.25f, kLayerDp, 0.f), Release::ToHalf);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 30.f, kLayerDp, 900.f), Release::ToHalf)
      << "a flick moves one stop";
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, way * 0.25f - 1.f, kLayerDp, 100.f), Release::Stay);
}

TEST(PageSheetModelTest, FromFullADragDownGoesBackToLarge) {
  const float way = kLargeTop - kFullTop;
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, way * 0.25f, kLayerDp, 0.f), Release::ToLarge);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 30.f, kLayerDp, 3000.f), Release::ToLarge);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, 20.f, kLayerDp, 100.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, -200.f, kLayerDp, -3000.f), Release::Stay)
      << "nothing above full";
}

TEST(PageSheetModelTest, FromHalfAQuarterDownOrAFlickDownClosesAndUpGoesBackToLarge) {
  const float height = kLayerDp - kHalfTop;
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, height * 0.25f, kLayerDp, 0.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, 40.f, kLayerDp, 900.f), Release::Dismiss);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, 30.f, kLayerDp, 100.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, -(kHalfTop - kLargeTop) * 0.25f, kLayerDp, 0.f),
            Release::ToLarge);
}

TEST(PageSheetModelTest, ALongDragPassesMoreThanOneStop) {
  // From large past half by more than a quarter of the half sheet: closed in one go.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, (kHalfTop - kLargeTop) + (kLayerDp - kHalfTop) * 0.3f,
                                      kLayerDp, 0.f),
            Release::Dismiss);
  // Just past half: it stops at half.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, (kHalfTop - kLargeTop) + 20.f, kLayerDp, 3000.f),
            Release::ToHalf)
      << "the flick counts for the first stop only";
  // From full down to past half's quarter: half.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Full, (kLargeTop - kFullTop) + (kHalfTop - kLargeTop) * 0.5f,
                                      kLayerDp, 0.f),
            Release::ToHalf);
  // From half up near the top: full.
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, -(kHalfTop - 20.f), kLayerDp, 0.f), Release::ToFull);
}

TEST(PageSheetModelTest, ATapThatWobblesIsNoFlick) {
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Half, 10.f, kLayerDp, 2000.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, -10.f, kLayerDp, -2000.f), Release::Stay);
  EXPECT_EQ(PageSheetModel::OnRelease(SheetDetent::Large, 0.f, kLayerDp, 5000.f), Release::Stay);
}

TEST(PageSheetModelTest, TheKeyboardShowsFullAndItsGoingBackRestoresTheStop) {
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Large, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Half, true), SheetDetent::Full);
  EXPECT_EQ(PageSheetModel::Shown(SheetDetent::Large, false), SheetDetent::Large) << "keyboard down: back to 4/5";
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
