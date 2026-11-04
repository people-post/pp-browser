// Phone collapsing bars (PLAN-ui-refresh-impl 原话 71). Kenneth: "那个稍微做宽一点，现在本来是毛玻璃的嘛，做宽一点，然后
// 向上向下滑的时候，把那毛玻璃收起来……就像苹果这种……而且这样的话会让聊天窗显示更多内容". The chat header is taller at rest
// and collapses while the user reads older history; it comes back towards the newest messages.
#include "gui/shell/NavBarCollapseModel.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

using Model = NavBarCollapseModel;

TEST(NavBarCollapseModelTest, TheChatBarIsTallerAtRest) {
  EXPECT_GE(Model::kChatBarDp, 56.f);
  EXPECT_LE(Model::kChatBarDp, 60.f);
  EXPECT_GE(Model::kChatBarCompactDp, 40.f);
  EXPECT_LE(Model::kChatBarCompactDp, 44.f);
}

TEST(NavBarCollapseModelTest, AChatOpensExpanded) {
  Model::ChatHeader header;
  EXPECT_FALSE(header.compact);
  EXPECT_FALSE(header.OnScroll(0.f));
  EXPECT_FALSE(header.compact);
}

TEST(NavBarCollapseModelTest, ScrollingIntoOlderHistoryPastTheThresholdCollapses) {
  Model::ChatHeader header;
  EXPECT_FALSE(header.OnScroll(10.f));
  EXPECT_FALSE(header.OnScroll(20.f)) << "a small move keeps it expanded";
  EXPECT_TRUE(header.OnScroll(Model::kChatCollapseDp)) << "24dp from the newest end it collapses";
  EXPECT_TRUE(header.compact);
  EXPECT_FALSE(header.OnScroll(400.f)) << "further into history it stays collapsed";
  EXPECT_TRUE(header.compact);
}

TEST(NavBarCollapseModelTest, ScrollingBackTowardsTheNewestPastTheThresholdExpands) {
  Model::ChatHeader header;
  header.OnScroll(600.f);
  ASSERT_TRUE(header.compact);
  EXPECT_FALSE(header.OnScroll(580.f)) << "a wobble back keeps it collapsed";
  EXPECT_TRUE(header.compact);
  EXPECT_TRUE(header.OnScroll(600.f - Model::kChatExpandDp)) << "32dp back towards the newest it expands";
  EXPECT_FALSE(header.compact);
}

TEST(NavBarCollapseModelTest, TheThresholdsCountFromWhereTheUserTurned) {
  Model::ChatHeader header;
  header.OnScroll(600.f);
  ASSERT_TRUE(header.compact);
  header.OnScroll(900.f);  // further up into history
  EXPECT_FALSE(header.OnScroll(880.f));
  EXPECT_TRUE(header.compact) << "back 20dp from the furthest point: still collapsed";
  EXPECT_TRUE(header.OnScroll(860.f));
  EXPECT_FALSE(header.compact) << "back 40dp from the furthest point: expanded";
  // Expanded in the middle of history: a turn back up into it collapses again from the nearest point.
  header.OnScroll(800.f);
  EXPECT_FALSE(header.compact);
  EXPECT_FALSE(header.OnScroll(820.f));
  EXPECT_TRUE(header.OnScroll(824.f));
  EXPECT_TRUE(header.compact);
}

TEST(NavBarCollapseModelTest, ReachingTheNewestEndAlwaysExpands) {
  Model::ChatHeader header;
  header.OnScroll(30.f);
  ASSERT_TRUE(header.compact);
  // 22dp back is short of the expand threshold, but it lands on the newest end, which always expands.
  EXPECT_TRUE(header.OnScroll(Model::kChatNewestEndDp));
  EXPECT_FALSE(header.compact);
}

TEST(NavBarCollapseModelTest, OnlyAFlipReports) {
  Model::ChatHeader header;
  int flips = 0;
  for (float d = 0.f; d <= 1000.f; d += 3.f) {
    flips += header.OnScroll(d) ? 1 : 0;
  }
  EXPECT_EQ(flips, 1) << "one steady scroll into history flips once, not on every event";
  for (float d = 1000.f; d >= 0.f; d -= 3.f) {
    flips += header.OnScroll(d) ? 1 : 0;
  }
  EXPECT_EQ(flips, 2);
}

TEST(NavBarCollapseModelTest, APushedPageKeepsItsLargeTitleUntilItScrolledUnderTheBar) {
  EXPECT_FALSE(Model::PageTitleCollapsed(0.f)) << "at rest (and on a page too short to scroll) the large title shows";
  EXPECT_FALSE(Model::PageTitleCollapsed(Model::kPageLargeTitleLineDp - 1.f));
  EXPECT_TRUE(Model::PageTitleCollapsed(Model::kPageLargeTitleLineDp)) << "its 34dp line is under the bar";
  EXPECT_TRUE(Model::PageTitleCollapsed(500.f));
  EXPECT_FALSE(Model::PageTitleCollapsed(10.f)) << "scrolled back, the large title returns";
}

TEST(NavBarCollapseModelTest, ResetStartsExpanded) {
  Model::ChatHeader header;
  header.OnScroll(500.f);
  ASSERT_TRUE(header.compact);
  header.Reset();
  EXPECT_FALSE(header.compact);
  EXPECT_FLOAT_EQ(header.turn_dp, 0.f);
}

} // namespace
} // namespace pbr
