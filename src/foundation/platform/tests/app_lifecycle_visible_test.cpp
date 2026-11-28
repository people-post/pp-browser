#include "foundation/runtime/AppLifecycle.h"

#include "common/runtime/AppRuntime.h"

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

// The attentive listener: the user may be looking again. One notice per return, only when attention was lost.
class AppLifecycleAttentiveTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppLifecycle::OnDidEnterForeground();
    AppLifecycle::SetDesktopMinimized(false);
    AppLifecycle::SetDesktopInputFocused(true);
    id_ = AppLifecycle::AddAttentiveListener([this]() { ++fired_; });
  }
  void TearDown() override {
    AppLifecycle::RemoveListener(id_);
    AppLifecycle::OnDidEnterForeground();
    AppLifecycle::SetDesktopMinimized(false);
    AppLifecycle::SetDesktopInputFocused(true);
  }
  AppLifecycle::ListenerId id_ = 0;
  int fired_ = 0;
};

TEST_F(AppLifecycleAttentiveTest, FiresWhenFocusGoesFromLostToGainedOnly) {
  AppLifecycle::SetDesktopInputFocused(true); // no change
  EXPECT_EQ(fired_, 0);
  AppLifecycle::SetDesktopInputFocused(false);
  EXPECT_EQ(fired_, 0);
  AppLifecycle::SetDesktopInputFocused(true);
  EXPECT_EQ(fired_, 1);
  AppLifecycle::SetDesktopInputFocused(true); // again: no change
  EXPECT_EQ(fired_, 1);
}

TEST_F(AppLifecycleAttentiveTest, ARestoreThenAFocusGainIsOneReturn) {
  AppLifecycle::OnDesktopMinimized();
  AppLifecycle::OnDesktopRestored(); // restored but not focused: not attentive yet
  EXPECT_EQ(fired_, 0);
  AppLifecycle::SetDesktopInputFocused(true);
  EXPECT_EQ(fired_, 1);
}

TEST_F(AppLifecycleAttentiveTest, ARestoreWhenFocusCameFirstFiresOnTheRestore) {
  AppLifecycle::OnDesktopMinimized();
  AppLifecycle::SetDesktopInputFocused(true); // still minimized: not attentive
  EXPECT_EQ(fired_, 0);
  AppLifecycle::OnDesktopRestored();
  EXPECT_EQ(fired_, 1);
  AppLifecycle::OnDesktopRestored(); // a second restore event: nothing was lost since
  EXPECT_EQ(fired_, 1);
}

TEST_F(AppLifecycleAttentiveTest, ForegroundFiresOnlyWhenComingFromTheBackground) {
  AppLifecycle::OnDidEnterForeground(); // was already in front
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired_, 0);
  AppLifecycle::OnWillEnterBackground();
  EXPECT_EQ(fired_, 0);
  AppLifecycle::OnDidEnterForeground();
  EXPECT_EQ(fired_, 0) << "not inside the platform's lifecycle callback (#816)";
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired_, 1);
}

// #816: the foreground event comes inside SDL's event watch (its lock held). Its listeners reach into the mesh, whose io
// lock the mesh pump may hold while it posts to the UI (SDL's lock): they run on the next UI turn instead.
TEST(AppLifecycleForegroundTest, ListenersRunOnTheNextUiTurnNotInsideTheLifecycleCallback) {
  int fired = 0;
  const AppLifecycle::ListenerId id = AppLifecycle::AddForegroundListener([&fired]() { ++fired; });
  AppLifecycle::OnWillEnterBackground();
  AppLifecycle::OnDidEnterForeground();
  EXPECT_TRUE(AppLifecycle::IsForeground()) << "the state changes at once";
  EXPECT_EQ(fired, 0);
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 1);
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 1) << "once per return";
  AppLifecycle::RemoveListener(id);
}

TEST(AppLifecycleForegroundTest, ABackgroundBeforeTheTurnSkipsThemAndTheNextReturnRunsThemOnce) {
  int fired = 0;
  const AppLifecycle::ListenerId id = AppLifecycle::AddForegroundListener([&fired]() { ++fired; });
  AppLifecycle::OnWillEnterBackground();
  AppLifecycle::OnDidEnterForeground();
  AppLifecycle::OnWillEnterBackground(); // gone again before the UI turn
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 0) << "not run for an app that is back in the background";
  AppLifecycle::OnDidEnterForeground();
  AppLifecycle::OnWillEnterBackground();
  AppLifecycle::OnDidEnterForeground(); // two returns before one turn: the newest only
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 1);
  AppLifecycle::RemoveListener(id);
}

TEST_F(AppLifecycleAttentiveTest, ARemovedListenerIsNotCalled) {
  AppLifecycle::RemoveListener(id_);
  AppLifecycle::SetDesktopInputFocused(false);
  AppLifecycle::SetDesktopInputFocused(true);
  EXPECT_EQ(fired_, 0);
}

TEST(AppLifecycleRemoteNotificationTest, ListenersRunOnTheUiThreadViaPostUiUntilRemoved) {
  AppRuntime::Initialize(AppRuntimeConfig{});
  int fired = 0;
  const AppLifecycle::ListenerId id = AppLifecycle::AddRemoteNotificationListener([&fired]() { ++fired; });
  AppLifecycle::OnRemoteNotification();
  EXPECT_EQ(fired, 0) << "posted, not run inline";
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 1);
  AppLifecycle::RemoveListener(id);
  AppLifecycle::OnRemoteNotification();
  AppRuntime::RunUITasks();
  EXPECT_EQ(fired, 1);
  AppRuntime::Shutdown();
}

} // namespace
} // namespace pbr
