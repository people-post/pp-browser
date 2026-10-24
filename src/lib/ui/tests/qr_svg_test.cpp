#include "lib/ui/QrSvg.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(QrSvgTest, AnInviteBecomesDarkModulesOnWhite) {
  auto svg = QrSvgContent("pp-browser-contact-v1:eyJhY2NvdW50X2lkIjoiYWNjb3VudDp4In0");
  ASSERT_TRUE(svg) << svg.error().message;
  EXPECT_NE(svg->find("fill=\"#ffffff\""), std::string::npos);
  EXPECT_NE(svg->find("<path fill=\"#000000\""), std::string::npos);
  EXPECT_NE(svg->find("h1v1h-1z"), std::string::npos);
}

TEST(QrSvgTest, TextNoQrCodeHoldsIsRefused) {
  EXPECT_FALSE(QrSvgContent(std::string(4000, 'x')));
}

} // namespace
} // namespace pbr
