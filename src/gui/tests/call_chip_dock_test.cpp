// #166 (Kenneth 2026-11-08): the minimised call chip rests where it is dropped, as the phone AI dot does — the nearer
// side, the height kept in a band (12dp from the top, 72dp from the bottom, as the old corner classes).
#include "lib/ui/CallChipDock.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// A phone's call layer (390×780dp) and a typical chip (180×56dp): the band for its top is [12, 780 - 72 - 56 = 652].
constexpr CallChipArea kPhone{390.f, 780.f};
constexpr float kChipW = 180.f;
constexpr float kChipH = 56.f;

CallChipRect ChipAt(const float left, const float top) {
  return {left, top, kChipW, kChipH};
}

TEST(CallChipDockTest, TheNearerSideWins) {
  EXPECT_EQ(CallChipDock::Release(ChipAt(10.f, 300.f), kPhone).side, CallChipDock::kSideLeft);
  EXPECT_EQ(CallChipDock::Release(ChipAt(200.f, 300.f), kPhone).side, CallChipDock::kSideRight);
  // By the chip's centre: 100 + 90 = 190 < 195 is still the left half; 110 + 90 = 200 the right.
  EXPECT_EQ(CallChipDock::Release(ChipAt(100.f, 300.f), kPhone).side, CallChipDock::kSideLeft);
  EXPECT_EQ(CallChipDock::Release(ChipAt(110.f, 300.f), kPhone).side, CallChipDock::kSideRight);
  // Dragged half off either edge.
  EXPECT_EQ(CallChipDock::Release(ChipAt(-120.f, 300.f), kPhone).side, CallChipDock::kSideLeft);
  EXPECT_EQ(CallChipDock::Release(ChipAt(330.f, 300.f), kPhone).side, CallChipDock::kSideRight);
}

TEST(CallChipDockTest, TheBandIsTwelveFromTheTopAndSeventyTwoFromTheBottom) {
  EXPECT_FLOAT_EQ(CallChipDock::MinTop(), 12.f);
  EXPECT_FLOAT_EQ(CallChipDock::MaxTop(kPhone, kChipH), 780.f - 72.f - kChipH);

  // Let go above the band: at its top, where the default top-right chip sits (a light drag does not move it down).
  const CallChipPlace high = CallChipDock::Release(ChipAt(200.f, -40.f), kPhone);
  EXPECT_EQ(high.y_permille, 0);
  EXPECT_FLOAT_EQ(CallChipDock::Place(high, kPhone, kChipW, kChipH).top_dp, 12.f);
  EXPECT_EQ(CallChipDock::Release(ChipAt(200.f, 12.f), kPhone).y_permille, 0);

  // Let go below it: at its bottom, clear of the composer and the tab bar.
  const CallChipPlace low = CallChipDock::Release(ChipAt(10.f, 760.f), kPhone);
  EXPECT_EQ(low.y_permille, 1000);
  EXPECT_FLOAT_EQ(CallChipDock::Place(low, kPhone, kChipW, kChipH).top_dp, 652.f);
}

TEST(CallChipDockTest, ItRestsTwelveFromItsSide) {
  const CallChipPoint left = CallChipDock::Place({CallChipDock::kSideLeft, 500}, kPhone, kChipW, kChipH);
  EXPECT_FLOAT_EQ(left.left_dp, 12.f);
  const CallChipPoint right = CallChipDock::Place({CallChipDock::kSideRight, 500}, kPhone, kChipW, kChipH);
  EXPECT_FLOAT_EQ(right.left_dp, 390.f - 12.f - kChipW);
  EXPECT_FLOAT_EQ(right.top_dp, 12.f + (652.f - 12.f) * 0.5f);
}

TEST(CallChipDockTest, PlaceLandsWithinOnePermilleOfTheDropHeight) {
  for (float top = 12.f; top <= 652.f; top += 7.3f) {
    const CallChipPlace place = CallChipDock::Release(ChipAt(30.f, top), kPhone);
    const float placed = CallChipDock::Place(place, kPhone, kChipW, kChipH).top_dp;
    EXPECT_LE(std::abs(placed - top), (652.f - 12.f) / 1000.f) << "dropped at " << top;
    // And the share round-trips: placed there again, it is the same place.
    EXPECT_EQ(CallChipDock::Release(ChipAt(30.f, placed), kPhone), place) << "dropped at " << top;
  }
}

TEST(CallChipDockTest, ATallerChipOrAShorterWindowStaysInTheBand) {
  const CallChipPlace bottom{CallChipDock::kSideRight, 1000};
  const CallChipPlace middle{CallChipDock::kSideRight, 500};

  // The chip grows (the quality words, a second line): its bottom stays 72dp above the layer's.
  const float taller = 80.f;
  EXPECT_FLOAT_EQ(CallChipDock::Place(bottom, kPhone, kChipW, taller).top_dp + taller, 780.f - 72.f);
  EXPECT_LE(CallChipDock::Place(middle, kPhone, kChipW, taller).top_dp + taller, 780.f - 72.f);

  // Landscape, or a desktop window made short: the same share, in the smaller band.
  const CallChipArea landscape{780.f, 360.f};
  const float top = CallChipDock::Place(bottom, landscape, kChipW, kChipH).top_dp;
  EXPECT_GE(top, 12.f);
  EXPECT_FLOAT_EQ(top + kChipH, 360.f - 72.f);
  const float mid = CallChipDock::Place(middle, landscape, kChipW, kChipH).top_dp;
  EXPECT_GE(mid, 12.f);
  EXPECT_LE(mid + kChipH, 360.f - 72.f);

  // Narrower: the right side follows the edge.
  EXPECT_FLOAT_EQ(CallChipDock::Place(middle, {320.f, 780.f}, kChipW, kChipH).left_dp, 320.f - 12.f - kChipW);

  // Too short for any band: pinned at its top, never above it.
  const CallChipArea tiny{390.f, 100.f};
  EXPECT_FLOAT_EQ(CallChipDock::Place(bottom, tiny, kChipW, kChipH).top_dp, 12.f);
  EXPECT_EQ(CallChipDock::Release(ChipAt(10.f, 50.f), tiny).y_permille, 0);
}

TEST(CallChipDockTest, OutOfRangeSharesAreClamped) {
  EXPECT_FLOAT_EQ(CallChipDock::Place({CallChipDock::kSideLeft, -5}, kPhone, kChipW, kChipH).top_dp, 12.f);
  EXPECT_FLOAT_EQ(CallChipDock::Place({CallChipDock::kSideLeft, 4000}, kPhone, kChipW, kChipH).top_dp, 652.f);
}

// #189: the handle: a 44dp circle with 12dp of it beyond the edge it hangs on, at the band height like the bar.
TEST(CallChipDockTest, TheHandleHangsOnItsEdgeAndUsesTheWholeBand) {
  constexpr float kH = CallChipDock::kHandleDp;
  EXPECT_FLOAT_EQ(kH, 44.f);
  const CallChipPlace right{CallChipDock::kSideRight, 0, true};
  const CallChipPlace left{CallChipDock::kSideLeft, 1000, true};
  EXPECT_FLOAT_EQ(CallChipDock::Place(right, kPhone, kH, kH).left_dp, 390.f - kH + 12.f);
  EXPECT_FLOAT_EQ(CallChipDock::Place(left, kPhone, kH, kH).left_dp, -12.f);
  EXPECT_FLOAT_EQ(CallChipDock::Place(left, kPhone, kH, kH).top_dp, 780.f - 72.f - kH);
  EXPECT_FALSE((CallChipPlace{CallChipDock::kSideRight, 0, true} == CallChipPlace{CallChipDock::kSideRight, 0, false}));
}

} // namespace
} // namespace pbr
