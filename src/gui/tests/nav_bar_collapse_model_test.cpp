// Phone bars that hide on scroll (PLAN-ui-refresh-impl 原话 78, 80). Kenneth: "其实滚动的时候，这个这个这个顶栏应该就
// 收回去，就不要显示了。那个聊天窗也是这样的……应该是全部收回去，就不显示了". The chat header and a pushed page's bar
// slide away while the user scrolls away from home (a chat's newest message, a page's top) and come back towards it.
#include "gui/shell/NavBarCollapseModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Model = NavBarCollapseModel;

TEST(NavBarCollapseModelTest, ABarOpensShown) {
  Model::Bar bar;
  EXPECT_FALSE(bar.hidden);
  EXPECT_FALSE(bar.OnScroll(0.f));
  EXPECT_FALSE(bar.hidden);
}

TEST(NavBarCollapseModelTest, ScrollingAwayFromHomePastTheThresholdHides) {
  Model::Bar bar;
  EXPECT_FALSE(bar.OnScroll(10.f));
  EXPECT_FALSE(bar.OnScroll(20.f)) << "a small move keeps it";
  EXPECT_TRUE(bar.OnScroll(Model::kHideDp)) << "24dp from home it hides";
  EXPECT_TRUE(bar.hidden);
  EXPECT_FALSE(bar.OnScroll(400.f)) << "further away it stays hidden";
  EXPECT_TRUE(bar.hidden);
}

TEST(NavBarCollapseModelTest, ScrollingBackTowardsHomePastTheThresholdShows) {
  Model::Bar bar;
  bar.OnScroll(600.f);
  ASSERT_TRUE(bar.hidden);
  EXPECT_FALSE(bar.OnScroll(580.f)) << "a wobble back keeps it hidden";
  EXPECT_TRUE(bar.hidden);
  EXPECT_TRUE(bar.OnScroll(600.f - Model::kShowDp)) << "32dp back towards home it shows";
  EXPECT_FALSE(bar.hidden);
}

TEST(NavBarCollapseModelTest, TheThresholdsCountFromWhereTheUserTurned) {
  Model::Bar bar;
  bar.OnScroll(600.f);
  ASSERT_TRUE(bar.hidden);
  bar.OnScroll(900.f);  // further away
  EXPECT_FALSE(bar.OnScroll(880.f));
  EXPECT_TRUE(bar.hidden) << "back 20dp from the furthest point: still hidden";
  EXPECT_TRUE(bar.OnScroll(860.f));
  EXPECT_FALSE(bar.hidden) << "back 40dp from the furthest point: shown";
  // Shown in the middle: a turn away again hides it from the nearest point.
  bar.OnScroll(800.f);
  EXPECT_FALSE(bar.hidden);
  EXPECT_FALSE(bar.OnScroll(820.f));
  EXPECT_TRUE(bar.OnScroll(824.f));
  EXPECT_TRUE(bar.hidden);
}

TEST(NavBarCollapseModelTest, ReachingHomeAlwaysShows) {
  Model::Bar bar;
  bar.OnScroll(30.f);
  ASSERT_TRUE(bar.hidden);
  // 22dp back is short of the show threshold, but it lands on home, which always shows the bar.
  EXPECT_TRUE(bar.OnScroll(Model::kHomeEndDp));
  EXPECT_FALSE(bar.hidden);
}

TEST(NavBarCollapseModelTest, OnlyAFlipReports) {
  Model::Bar bar;
  int flips = 0;
  for (float d = 0.f; d <= 1000.f; d += 3.f) {
    flips += bar.OnScroll(d) ? 1 : 0;
  }
  EXPECT_EQ(flips, 1) << "one steady scroll away flips once, not on every event";
  for (float d = 1000.f; d >= 0.f; d -= 3.f) {
    flips += bar.OnScroll(d) ? 1 : 0;
  }
  EXPECT_EQ(flips, 2);
}

TEST(NavBarCollapseModelTest, APageBarIsClearAtHomeAndTakesItsMaterialOnceContentIsUnderIt) {
  EXPECT_FALSE(Model::ContentUnderBar(0.f));
  EXPECT_FALSE(Model::ContentUnderBar(Model::kHomeEndDp));
  EXPECT_TRUE(Model::ContentUnderBar(Model::kHomeEndDp + 1.f));
}

TEST(NavBarCollapseModelTest, ResetStartsShown) {
  Model::Bar bar;
  bar.OnScroll(500.f);
  ASSERT_TRUE(bar.hidden);
  bar.Reset();
  EXPECT_FALSE(bar.hidden);
  EXPECT_FLOAT_EQ(bar.turn_dp, 0.f);
}

} // namespace
} // namespace pbr
