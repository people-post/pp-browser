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
  const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
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

TEST(QrSvgTest, APngOfTextNoQrCodeHoldsIsRefused) {
  EXPECT_FALSE(QrPngBytes(std::string(4000, 'x')));
}

TEST(QrSvgTest, TextNoQrCodeHoldsIsRefused) {
  EXPECT_FALSE(QrSvgContent(std::string(4000, 'x')));
}

} // namespace
} // namespace pbr
