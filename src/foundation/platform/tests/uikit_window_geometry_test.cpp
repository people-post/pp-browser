#include "foundation/platform/UikitWindowGeometry.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

UikitViewGeometry Matching() {
  UikitViewGeometry g;
  g.window_w = g.view_w = 1373;
  g.window_h = g.view_h = 954;
  g.view_scale = g.screen_scale = 1.719;
  return g;
}

TEST(UikitWindowGeometryTest, MatchingViewNeedsNothing) {
  EXPECT_FALSE(PlanUikitGeometryFix(Matching()).Any());
}

TEST(UikitWindowGeometryTest, PortraitViewInLandscapeWindowIsResized) {
  // Launched in landscape: the view kept a portrait frame until the next rotation.
  UikitViewGeometry g = Matching();
  g.view_w = 954;
  g.view_h = 1373;
  const UikitGeometryFix fix = PlanUikitGeometryFix(g);
  EXPECT_TRUE(fix.resize_view);
  EXPECT_FALSE(fix.rescale_view);
}

TEST(UikitWindowGeometryTest, KeyboardTimeSwappedViewIsNotTheWindow) {
  // Logged 2026-11-16 on iPadOS 27: the keyboard came up in a 920x766 window and SDL gave its view 766x920. That size
  // is not drawn (UikitViewFillsWindow is false) and the view is put back.
  UikitViewGeometry g = Matching();
  g.window_w = 920;
  g.window_h = 766;
  g.view_w = 766;
  g.view_h = 920;
  EXPECT_TRUE(PlanUikitGeometryFix(g).resize_view);
}

TEST(UikitWindowGeometryTest, WindowResizedByTheUserIsFollowed) {
  UikitViewGeometry g = Matching();
  g.window_w = 900;
  g.window_h = 700;
  EXPECT_TRUE(PlanUikitGeometryFix(g).resize_view);
}

TEST(UikitWindowGeometryTest, DrawableScaleFollowsTheScreenSdlMeasuresIn) {
  // The window moved to a screen with another native scale: SDL's pixel size and the drawable would disagree.
  UikitViewGeometry g = Matching();
  g.screen_scale = 2.0;
  const UikitGeometryFix fix = PlanUikitGeometryFix(g);
  EXPECT_TRUE(fix.rescale_view);
  EXPECT_FALSE(fix.resize_view);
}

TEST(UikitWindowGeometryTest, SubPointDifferencesAreIgnored) {
  UikitViewGeometry g = Matching();
  g.view_w += 0.3;
  g.view_scale += 0.0005;
  EXPECT_FALSE(PlanUikitGeometryFix(g).Any());
}

TEST(UikitWindowGeometryTest, WindowWithoutSizeIsLeftAlone) {
  UikitViewGeometry g = Matching();
  g.window_w = 0;
  g.window_h = 0;
  EXPECT_FALSE(PlanUikitGeometryFix(g).Any());
}

TEST(UikitWindowGeometryTest, DesktopHelpersAreNoOps) {
  EXPECT_FALSE(SyncUikitWindowGeometry(nullptr).Any());
  EXPECT_TRUE(UikitViewFillsWindow(nullptr));
  EXPECT_EQ(WindowControlsTopInset(nullptr), 0);
  EXPECT_TRUE(DescribeUikitWindowGeometry(nullptr).empty());
  EXPECT_FALSE(IsIpadDevice());
}

TEST(UikitWindowGeometryTest, IpadMinimumWindowIsNoNarrowerThanAnIphone) {
  EXPECT_EQ(kIpadMinWindowWidthPt, 375);
  EXPECT_EQ(kIpadMinWindowHeightPt, 500);
}

} // namespace
} // namespace pbr
