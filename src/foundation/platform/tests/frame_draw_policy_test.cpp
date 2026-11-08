#include "foundation/platform/ui/FrameDrawPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(FrameDrawPolicyTest, PhonesDrawOnlyInTheForeground) {
  EXPECT_TRUE(ShouldDrawFrame(/*is_mobile=*/true, /*is_foreground=*/true));
  EXPECT_FALSE(ShouldDrawFrame(/*is_mobile=*/true, /*is_foreground=*/false));
}

TEST(FrameDrawPolicyTest, DesktopsAlwaysDraw) {
  EXPECT_TRUE(ShouldDrawFrame(/*is_mobile=*/false, /*is_foreground=*/true));
  EXPECT_TRUE(ShouldDrawFrame(/*is_mobile=*/false, /*is_foreground=*/false));
}

} // namespace
} // namespace pbr
