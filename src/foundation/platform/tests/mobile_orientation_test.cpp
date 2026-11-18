#include "foundation/platform/MobileOrientation.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(MobileOrientationTest, PhoneStaysInPortrait) {
  EXPECT_STREQ(PhoneOrientationsHint(), "Portrait");
}

TEST(MobileOrientationTest, TabletTakesEveryOrientation) {
  const std::string hint = TabletOrientationsHint();
  for (const char* name : {"Portrait", "PortraitUpsideDown", "LandscapeLeft", "LandscapeRight"}) {
    EXPECT_NE(hint.find(name), std::string::npos) << name;
  }
}

TEST(MobileOrientationTest, DesktopSetsNoHint) {
  // Desktop test hosts: the window resizes freely, so no orientation hint is set.
  EXPECT_EQ(MobileOrientationsHint(), nullptr);
}

} // namespace
} // namespace pbr
