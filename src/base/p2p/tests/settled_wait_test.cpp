#include "base/p2p/SettledWait.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

TEST(SettledWaitTest, FinishOnceReturnsValue) {
  SettledWait<int> wait;
  EXPECT_TRUE(wait.Finish(7));
  EXPECT_FALSE(wait.Finish(8));
  auto result = wait.Wait(std::chrono::milliseconds(1), Error("timed out"));
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  EXPECT_EQ(*result, 7);
}

TEST(SettledWaitTest, TimeoutFinishesAndIgnoresLateValue) {
  SettledWait<int> wait;
  auto result = wait.Wait(std::chrono::milliseconds(1), Error("timed out"));
  EXPECT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().message, "timed out");
  EXPECT_FALSE(wait.Finish(9));
}

TEST(SettledWaitTest, CopySharesState) {
  SettledWait<int> wait;
  SettledWait<int> copy = wait;
  EXPECT_TRUE(copy.Finish(3));
  auto result = wait.Wait(std::chrono::milliseconds(1), Error("timed out"));
  ASSERT_TRUE(static_cast<bool>(result));
  EXPECT_EQ(*result, 3);
}

} // namespace
