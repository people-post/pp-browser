#include "foundation/platform/ui/FrameDrawPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(FrameDrawPolicyTest, AndroidDrawsOnlyInTheForeground) {
  EXPECT_TRUE(ShouldDrawFrame(/*is_android=*/true, /*is_foreground=*/true));
  EXPECT_FALSE(ShouldDrawFrame(/*is_android=*/true, /*is_foreground=*/false));
}

TEST(FrameDrawPolicyTest, OtherPlatformsAlwaysDraw) {
  EXPECT_TRUE(ShouldDrawFrame(/*is_android=*/false, /*is_foreground=*/true));
  EXPECT_TRUE(ShouldDrawFrame(/*is_android=*/false, /*is_foreground=*/false));
}

} // namespace
} // namespace pbr
