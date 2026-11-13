// The screen-awake switch is called only on a change (B215).
#include "gui/ScreenAwakeSync.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

struct Calls {
  std::vector<bool> seen;
  ScreenAwakeSync sync{[this](const bool on) { seen.push_back(on); }};
};

TEST(ScreenAwakeSyncTest, SameValueManyTimesNeverCallsTheSwitchWhenOff) {
  Calls c;
  for (int i = 0; i < 100; ++i) {
    c.sync.Set(false);
  }
  EXPECT_TRUE(c.seen.empty());
}

TEST(ScreenAwakeSyncTest, OffToOnCallsOnceWithOn) {
  Calls c;
  EXPECT_TRUE(c.sync.Set(true));
  ASSERT_EQ(c.seen.size(), 1u);
  EXPECT_TRUE(c.seen[0]);
}

TEST(ScreenAwakeSyncTest, StayingOnDoesNotCallAgain) {
  Calls c;
  c.sync.Set(true);
  for (int i = 0; i < 50; ++i) {
    EXPECT_FALSE(c.sync.Set(true));
  }
  EXPECT_EQ(c.seen.size(), 1u);
}

TEST(ScreenAwakeSyncTest, OnToOffCallsOnceWithOff) {
  Calls c;
  c.sync.Set(true);
  c.sync.Set(false);
  ASSERT_EQ(c.seen.size(), 2u);
  EXPECT_FALSE(c.seen[1]);
}

TEST(ScreenAwakeSyncTest, ForceOffCallsOnceWhenOnAndNeverWhenOff) {
  Calls c;
  EXPECT_FALSE(c.sync.ForceOff());
  EXPECT_TRUE(c.seen.empty());
  c.sync.Set(true);
  EXPECT_TRUE(c.sync.ForceOff());
  EXPECT_FALSE(c.sync.ForceOff());
  ASSERT_EQ(c.seen.size(), 2u);
  EXPECT_FALSE(c.seen[1]);
  EXPECT_FALSE(c.sync.On());
}

} // namespace
} // namespace pbr
