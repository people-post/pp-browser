// The screen-awake switch is called only on a change, and turning off waits out a flap (B215).
#include "gui/ScreenAwakeSync.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

constexpr int64_t kDebounce = ScreenAwakeSync::kOffDebounceMs;

struct Calls {
  std::vector<bool> seen;
  ScreenAwakeSync sync{[this](const bool on) { seen.push_back(on); }};
};

TEST(ScreenAwakeSyncTest, SameValueManyTimesNeverCallsTheSwitchWhenOff) {
  Calls c;
  for (int i = 0; i < 100; ++i) {
    c.sync.Set(false, i * 100);
  }
  EXPECT_TRUE(c.seen.empty());
}

TEST(ScreenAwakeSyncTest, OffToOnCallsOnceWithOnImmediately) {
  Calls c;
  EXPECT_TRUE(c.sync.Set(true, 0));
  ASSERT_EQ(c.seen.size(), 1u);
  EXPECT_TRUE(c.seen[0]);
}

TEST(ScreenAwakeSyncTest, StayingOnDoesNotCallAgain) {
  Calls c;
  c.sync.Set(true, 0);
  for (int i = 1; i < 50; ++i) {
    EXPECT_FALSE(c.sync.Set(true, i * 100));
  }
  EXPECT_EQ(c.seen.size(), 1u);
}

TEST(ScreenAwakeSyncTest, OffRequestIsHeldUntilTheDebounceHasPassed) {
  Calls c;
  c.sync.Set(true, 0);
  EXPECT_FALSE(c.sync.Set(false, 1000));
  EXPECT_FALSE(c.sync.Set(false, 1000 + kDebounce - 1));
  EXPECT_TRUE(c.sync.On());
  EXPECT_TRUE(c.sync.Set(false, 1000 + kDebounce));
  ASSERT_EQ(c.seen.size(), 2u);
  EXPECT_FALSE(c.seen[1]);
  EXPECT_FALSE(c.sync.On());
}

TEST(ScreenAwakeSyncTest, OffThenOnWithinTheDebounceNeverCallsTheSwitch) {
  Calls c;
  c.sync.Set(true, 0);
  c.sync.Set(false, 1000);
  c.sync.Set(false, 2000);
  EXPECT_FALSE(c.sync.Set(true, 1000 + kDebounce - 1));
  EXPECT_EQ(c.seen.size(), 1u); // only the first on
  EXPECT_TRUE(c.sync.On());
}

TEST(ScreenAwakeSyncTest, AFlapRestartsTheDebounceWindow) {
  Calls c;
  c.sync.Set(true, 0);
  c.sync.Set(false, 1000);
  c.sync.Set(true, 2500); // back before the window ended
  EXPECT_FALSE(c.sync.Set(false, 3000));
  EXPECT_FALSE(c.sync.Set(false, 3000 + kDebounce - 1)); // counted from 3000, not from 1000
  EXPECT_TRUE(c.sync.Set(false, 3000 + kDebounce));
  EXPECT_EQ(c.seen.size(), 2u);
}

TEST(ScreenAwakeSyncTest, ForceOffIsImmediateAndNeverCallsWhenOff) {
  Calls c;
  EXPECT_FALSE(c.sync.ForceOff());
  EXPECT_TRUE(c.seen.empty());
  c.sync.Set(true, 0);
  c.sync.Set(false, 10); // a pending off must not delay it
  EXPECT_TRUE(c.sync.ForceOff());
  EXPECT_FALSE(c.sync.ForceOff());
  ASSERT_EQ(c.seen.size(), 2u);
  EXPECT_FALSE(c.seen[1]);
  EXPECT_FALSE(c.sync.On());
}

} // namespace
} // namespace pbr
