// Home's 关注联系人 row (PLAN-ui-refresh-impl 原话 75-77). Kenneth: "一排4个正好排满整整个屏幕不会从左边或者右边突出半个
// 出来……但是不是说一夜翻4个哦，就是还是还还是有个滚动的过程，只是说停下来的时候，正好就是4个站满了".
#include "gui/shell/FavoritesRowModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Model = FavoritesRowModel;

TEST(FavoritesRowModelTest, TheAvatarFollowsTheScreenWidthWithinItsLimits) {
  EXPECT_EQ(Model::AvatarDp(393.f), 64) << "an iPhone 16 Pro";
  EXPECT_EQ(Model::AvatarDp(430.f), 71) << "a Pro Max";
  EXPECT_EQ(Model::AvatarDp(320.f), 56) << "a small phone: never below 56";
  EXPECT_EQ(Model::AvatarDp(600.f), 72) << "a wide window: never above 72";
}

TEST(FavoritesRowModelTest, FourOrFewerDoNotScroll) {
  EXPECT_FLOAT_EQ(Model::SnapOffset(0.f, 80.f, 3), 0.f);
  EXPECT_FLOAT_EQ(Model::SnapOffset(37.f, 80.f, 4), 0.f);
  EXPECT_FLOAT_EQ(Model::SnapOffset(37.f, 0.f, 6), 0.f) << "not laid out yet";
}

TEST(FavoritesRowModelTest, AtRestTheRowEasesToTheNearestPerson) {
  constexpr float kCell = 80.f;
  EXPECT_FLOAT_EQ(Model::SnapOffset(39.f, kCell, 6), 0.f) << "less than half a person: back";
  EXPECT_FLOAT_EQ(Model::SnapOffset(41.f, kCell, 6), 80.f) << "more than half: on to the next";
  EXPECT_FLOAT_EQ(Model::SnapOffset(80.f, kCell, 6), 80.f) << "already on a boundary: stays";
  EXPECT_FLOAT_EQ(Model::SnapOffset(130.f, kCell, 12), 160.f) << "a long coast ends anywhere; still a whole person";
}

TEST(FavoritesRowModelTest, TheTargetStaysWithinTheScrollRange) {
  constexpr float kCell = 80.f;
  // Six people: the last four show at 2 cells; a rubber band past either end comes back to it.
  EXPECT_FLOAT_EQ(Model::SnapOffset(175.f, kCell, 6), 160.f);
  EXPECT_FLOAT_EQ(Model::SnapOffset(-12.f, kCell, 6), 0.f);
  EXPECT_FLOAT_EQ(Model::SnapOffset(900.f, kCell, 12), 640.f);
}

} // namespace
} // namespace pbr
