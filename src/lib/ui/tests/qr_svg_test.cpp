#include "lib/ui/QrSvg.h"

#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <qrcodegen.hpp>

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

TEST(QrSvgTest, ThePngIsTheSameCodeWithWholePixelModulesAndAQuietZone) {
  const std::string text = "pp-browser-contact-v1:eyJhY2NvdW50X2lkIjoiYWNjb3VudDp4In0";
  auto png = QrPngBytes(text);
  ASSERT_TRUE(png) << png.error().message;
  SDL_Surface* image = DecodeImageUpright(png->data(), png->size(), "png");
  ASSERT_NE(image, nullptr);
  const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const int span = code.getSize() + 8;
  const int scale = kQrPngTargetPx / span;
  EXPECT_EQ(image->w, span * scale);
  EXPECT_EQ(image->h, span * scale);
  EXPECT_GT(image->w, 700) << "big enough to scan from a screen";
  int wrong = 0;
  for (int y = 0; y < image->h; ++y) {
    for (int x = 0; x < image->w; ++x) {
      Uint8 r = 0, g = 0, b = 0, a = 0;
      ASSERT_TRUE(SDL_ReadSurfacePixel(image, x, y, &r, &g, &b, &a));
      const bool dark = code.getModule(x / scale - 4, y / scale - 4);
      if (r != (dark ? 0 : 255) || g != r || b != r) {
        ++wrong;
      }
    }
  }
  EXPECT_EQ(wrong, 0) << "every pixel is black or white as the code says; the border is white";
  SDL_DestroySurface(image);
}

TEST(QrSvgTest, LowErrorCorrectionMakesASmallerCodeForTheSameContactPayload) {
  const std::string text = "pp-browser-contact-v1:" + std::string(428, 'A'); // about the size of a real contact code
  ASSERT_EQ(text.size(), 450u);
  auto shape = QrSvgShape(text);
  ASSERT_TRUE(shape) << shape.error().message;
  const auto low = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
  const auto medium = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  EXPECT_EQ(shape->version, low.getVersion());
  EXPECT_EQ(shape->modules, low.getSize());
  EXPECT_EQ(shape->bytes, 450);
  EXPECT_LT(shape->version, medium.getVersion());
  EXPECT_LT(shape->modules, medium.getSize());
  // The SVG draws exactly that code: the dark modules of the L code, nothing else.
  auto svg = QrSvgContent(text);
  ASSERT_TRUE(svg);
  int dark = 0;
  for (int y = 0; y < low.getSize(); ++y) {
    for (int x = 0; x < low.getSize(); ++x) {
      dark += low.getModule(x, y) ? 1 : 0;
    }
  }
  int drawn = 0;
  for (size_t at = svg->find("h1v1h-1z"); at != std::string::npos; at = svg->find("h1v1h-1z", at + 1)) {
    ++drawn;
  }
  EXPECT_EQ(drawn, dark);
}

TEST(QrSvgTest, AQrCodeOnAPhoneIsAsWideAsThePageAllowsWithTwentyDpEachSideAtMost340) {
  EXPECT_EQ(QrPhoneDisplayDp(360.f), 320);
  EXPECT_EQ(QrPhoneDisplayDp(393.f), 340);
  EXPECT_EQ(QrPhoneDisplayDp(375.f), 335);
  EXPECT_EQ(QrPhoneDisplayDp(320.f), 280);
  EXPECT_EQ(QrPhoneDisplayDp(430.f), 340);
  EXPECT_EQ(QrPhoneDisplayDp(10.f), 1);
}

TEST(QrSvgTest, TheQuietZoneStaysFourModulesAndTheCodeIsBlackOnWhiteWhateverTheTheme) {
  auto svg = QrSvgContent("hello");
  ASSERT_TRUE(svg);
  EXPECT_NE(svg->find("<rect width=\"100\" height=\"100\" fill=\"#ffffff\"/>"), std::string::npos);
  EXPECT_NE(svg->find("M4 4h1v1h-1z"), std::string::npos); // the top-left finder corner sits 4 modules in
}

TEST(QrSvgTest, APngOfTextNoQrCodeHoldsIsRefused) {
  EXPECT_FALSE(QrPngBytes(std::string(4000, 'x')));
}

TEST(QrSvgTest, TextNoQrCodeHoldsIsRefused) {
  EXPECT_FALSE(QrSvgContent(std::string(4000, 'x')));
}

} // namespace
} // namespace pbr
