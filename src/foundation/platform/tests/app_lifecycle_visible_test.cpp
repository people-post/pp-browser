#include "foundation/runtime/AppLifecycle.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Where a call rings follows IsVisible: an overlay (Control Center, a system call screen) resigns the app
// active (SDL's WILL_ENTER_BACKGROUND) but leaves its screen up; only the background proper hides it.
class AppLifecycleVisibleTest : public ::testing::Test {
protected:
  void TearDown() override { AppLifecycle::OnDidEnterForeground(); }
};

TEST_F(AppLifecycleVisibleTest, AnOverlayLeavesTheAppVisible) {
  ASSERT_TRUE(AppLifecycle::IsVisible());
  AppLifecycle::OnWillEnterBackground();
  EXPECT_FALSE(AppLifecycle::IsForeground());
  EXPECT_TRUE(AppLifecycle::IsVisible()) << "resigned active only: the app still rings itself";
  AppLifecycle::OnDidEnterForeground();
  EXPECT_TRUE(AppLifecycle::IsForeground());
  EXPECT_TRUE(AppLifecycle::IsVisible());
}

TEST_F(AppLifecycleVisibleTest, TheBackgroundProperHidesItUntilItComesBack) {
  AppLifecycle::OnWillEnterBackground();
  AppLifecycle::OnDidEnterBackground();
  EXPECT_FALSE(AppLifecycle::IsVisible());
  EXPECT_FALSE(AppLifecycle::IsForeground());
  AppLifecycle::OnDidEnterForeground();
  EXPECT_TRUE(AppLifecycle::IsVisible());
  EXPECT_TRUE(AppLifecycle::IsForeground());
}

} // namespace
} // namespace pbr
