#include "foundation/platform/PlatformLifecycleEvents.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

// The two real edges reach their own listeners only, in order, and a removed listener hears nothing.
TEST(PlatformLifecycleEventsTest, EachEdgeReachesItsOwnListeners) {
  std::vector<int> seen;
  const auto bg = PlatformLifecycleEvents::AddEnteredBackgroundListener([&]() { seen.push_back(1); });
  const auto fg = PlatformLifecycleEvents::AddWillEnterForegroundListener([&]() { seen.push_back(2); });
  PlatformLifecycleEvents::OnDidEnterBackground();
  PlatformLifecycleEvents::OnDidEnterForeground(); // no active listener yet: nothing
  PlatformLifecycleEvents::OnWillEnterForeground();
  EXPECT_EQ(seen, (std::vector<int>{1, 2}));
  const auto active = PlatformLifecycleEvents::AddDidEnterForegroundListener([&]() { seen.push_back(3); });
  PlatformLifecycleEvents::OnDidEnterForeground();
  PlatformLifecycleEvents::OnDidEnterForeground(); // every time, never gated
  EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 3}));
  PlatformLifecycleEvents::RemoveListener(active);
  seen.resize(2);
  PlatformLifecycleEvents::RemoveListener(bg);
  PlatformLifecycleEvents::OnDidEnterBackground();
  PlatformLifecycleEvents::OnWillEnterForeground();
  EXPECT_EQ(seen, (std::vector<int>{1, 2, 2}));
  PlatformLifecycleEvents::RemoveListener(fg);
}

TEST(PlatformLifecycleEventsTest, MonoMsNeverGoesBack) {
  const int64_t a = PlatformLifecycleEvents::MonoMs();
  const int64_t b = PlatformLifecycleEvents::MonoMs();
  EXPECT_LE(a, b);
}

} // namespace
} // namespace pbr
