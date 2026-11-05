// The PIN the UI demo app unlocks with at bootstrap (DEV/PLAN-ui-demo-data.md); always compiled,
// so the normal build runs it.
#include "feature/product/UiDemoConfig.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(UiDemoConfigTest, TheDemoUsesItsFixedPinAndTheRealAppKeepsWhatItWasGiven) {
  EXPECT_EQ(ResolveBootstrapPin(true, ""), "123456");
  EXPECT_EQ(ResolveBootstrapPin(true, "999999"), "123456");
  EXPECT_EQ(ResolveBootstrapPin(false, "999999"), "999999");
  EXPECT_EQ(ResolveBootstrapPin(false, ""), "");
}

} // namespace
} // namespace pbr
