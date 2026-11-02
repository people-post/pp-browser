#include "lib/net/PublicOnlySocket.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr uint32_t Ip(const int a, const int b, const int c, const int d) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) |
         static_cast<uint32_t>(d);
}

TEST(PublicOnlySocketTest, ALocalProxysFakeIpIsAllowedBecauseTheProxyDoesTheRouting) {
  EXPECT_TRUE(PublicOnlySocketAllowsIPv4(Ip(198, 18, 0, 80)));  // Clash fake-ip
  EXPECT_TRUE(PublicOnlySocketAllowsIPv4(Ip(198, 19, 255, 1))); // top of the /15
}

TEST(PublicOnlySocketTest, PublicHostsStayAllowedAndPrivateOrTestRangesStayRefused) {
  EXPECT_TRUE(PublicOnlySocketAllowsIPv4(Ip(8, 8, 8, 8)));
  EXPECT_FALSE(PublicOnlySocketAllowsIPv4(Ip(10, 0, 0, 1)));
  EXPECT_FALSE(PublicOnlySocketAllowsIPv4(Ip(127, 0, 0, 1)));
  EXPECT_FALSE(PublicOnlySocketAllowsIPv4(Ip(192, 168, 0, 105)));
  EXPECT_FALSE(PublicOnlySocketAllowsIPv4(Ip(198, 51, 100, 1))); // TEST-NET-2, right next to the fake-ip range
  EXPECT_TRUE(PublicOnlySocketAllowsIPv4(Ip(198, 20, 0, 1)));    // just past the /15: ordinary public space
}

} // namespace
} // namespace pbr
