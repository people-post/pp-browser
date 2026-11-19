#include "foundation/platform/ScreenBrightnessRestore.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(ScreenBrightnessRestoreTest, UntouchedAfterOurRaiseIsGivenBack) {
  EXPECT_TRUE(ShouldRestoreScreenBrightness(1.0f, 1.0f));
  EXPECT_TRUE(ShouldRestoreScreenBrightness(0.99f, 1.0f)); // the OS may report a hair off
}

TEST(ScreenBrightnessRestoreTest, AChangeTheUserMadeWhileTheCodeWasShownIsLeftAlone) {
  EXPECT_FALSE(ShouldRestoreScreenBrightness(0.4f, 1.0f));
  EXPECT_FALSE(ShouldRestoreScreenBrightness(0.9f, 1.0f));
}

} // namespace
} // namespace pbr
