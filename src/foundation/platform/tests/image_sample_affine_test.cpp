// The avatar crop's sampler (PLAN-avatar-edit item 10): output pixels read from the source through an affine map,
// bilinear, the picture's edge repeated outside it.
#include "foundation/platform/ImageSampleAffine.h"

#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace pbr {
namespace {

struct Rgb {
  uint8_t r, g, b;
};

Rgb PixelAt(SDL_Surface* s, const int x, const int y) {
  Rgb c{};
  uint8_t a = 0;
  EXPECT_TRUE(SDL_ReadSurfacePixel(s, x, y, &c.r, &c.g, &c.b, &a));
  return c;
}

int MaxDiff(const Rgb a, const Rgb b) {
  return std::max({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// A pattern every pixel of which differs from its neighbours.
SDL_Surface* Pattern(const int w, const int h) {
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
  for (int y = 0; y < h; ++y) {
    auto* row = static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
    for (int x = 0; x < w; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(x & 0xFF);
      row[x * 3 + 1] = static_cast<uint8_t>(y & 0xFF);
      row[x * 3 + 2] = static_cast<uint8_t>((x * 7 + y * 13) % 256);
    }
  }
  return s;
}

// Top-left red, top-right green, bottom-left blue, bottom-right yellow.
SDL_Surface* Quadrants(const int w, const int h) {
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
  const auto fill = [&](const int x, const int y, const int fw, const int fh, const Rgb c) {
    const SDL_Rect r{x, y, fw, fh};
    SDL_FillSurfaceRect(s, &r, SDL_MapSurfaceRGB(s, c.r, c.g, c.b));
  };
  fill(0, 0, w / 2, h / 2, {255, 0, 0});
  fill(w / 2, 0, w - w / 2, h / 2, {0, 255, 0});
  fill(0, h / 2, w / 2, h - h / 2, {0, 0, 255});
  fill(w / 2, h / 2, w - w / 2, h - h / 2, {255, 255, 0});
  return s;
}

ImageAffine Translate(const double scale, const double x0, const double y0) {
  ImageAffine a;
  a.xu = scale;
  a.yv = scale;
  a.x0 = x0;
  a.y0 = y0;
  return a;
}

TEST(SampleAffineTest, TheIdentityCropIsTheCentredSquarePixelForPixel) {
  SDL_Surface* src = Pattern(300, 200);
  SDL_Surface* out = SampleAffine(src, 200, Translate(1.0, 50.0, 0.0));
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->w, 200);
  ASSERT_EQ(out->h, 200);
  int worst = 0;
  for (int y = 0; y < 200; y += 3) {
    for (int x = 0; x < 200; x += 3) {
      worst = std::max(worst, MaxDiff(PixelAt(out, x, y), PixelAt(src, x + 50, y)));
    }
  }
  EXPECT_EQ(worst, 0);
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

// The centred square scaled to N matches cropping with SDL and scaling with SDL's linear filter, within rounding (a
// smooth picture, so a half-pixel difference in where SDL puts its sample points stays within a level or two).
TEST(SampleAffineTest, TheIdentityCropScaledMatchesCropThenScale) {
  SDL_Surface* src = SDL_CreateSurface(300, 200, SDL_PIXELFORMAT_RGB24);
  for (int y = 0; y < 200; ++y) {
    auto* row = static_cast<uint8_t*>(src->pixels) + static_cast<size_t>(y) * src->pitch;
    for (int x = 0; x < 300; ++x) {
      row[x * 3 + 0] = static_cast<uint8_t>(x * 255 / 299);
      row[x * 3 + 1] = static_cast<uint8_t>(y * 255 / 199);
      row[x * 3 + 2] = static_cast<uint8_t>((x + y) * 255 / 498);
    }
  }
  SDL_Surface* crop = SDL_CreateSurface(200, 200, SDL_PIXELFORMAT_RGB24);
  const SDL_Rect centre{50, 0, 200, 200};
  ASSERT_TRUE(SDL_BlitSurface(src, &centre, crop, nullptr));
  SDL_Surface* want = SDL_ScaleSurface(crop, 100, 100, SDL_SCALEMODE_LINEAR);
  ASSERT_NE(want, nullptr);
  SDL_Surface* out = SampleAffine(src, 100, Translate(2.0, 50.0, 0.0));
  ASSERT_NE(out, nullptr);
  int worst = 0;
  for (int y = 0; y < 100; ++y) {
    for (int x = 0; x < 100; ++x) {
      worst = std::max(worst, MaxDiff(PixelAt(out, x, y), PixelAt(want, x, y)));
    }
  }
  EXPECT_LE(worst, 3);
  SDL_DestroySurface(out);
  SDL_DestroySurface(want);
  SDL_DestroySurface(crop);
  SDL_DestroySurface(src);
}

TEST(SampleAffineTest, AnOffsetPicksTheExpectedQuadrant) {
  SDL_Surface* src = Quadrants(200, 100);
  // The bottom-right quadrant is x 100..200, y 50..100: a 40 px square well inside it.
  SDL_Surface* out = SampleAffine(src, 40, Translate(1.0, 130.0, 55.0));
  ASSERT_NE(out, nullptr);
  for (int y = 0; y < 40; y += 3) {
    for (int x = 0; x < 40; x += 3) {
      EXPECT_EQ(MaxDiff(PixelAt(out, x, y), {255, 255, 0}), 0) << x << "," << y;
    }
  }
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

TEST(SampleAffineTest, OutsideThePictureTheNearestEdgePixelIsRepeated) {
  SDL_Surface* src = Pattern(50, 40);
  SDL_Surface* out = SampleAffine(src, 20, Translate(1.0, -500.0, 10.0)); // far to the left
  ASSERT_NE(out, nullptr);
  for (int y = 0; y < 20; ++y) {
    EXPECT_EQ(MaxDiff(PixelAt(out, 7, y), PixelAt(src, 0, y + 10)), 0) << y;
  }
  SDL_DestroySurface(out);
  out = SampleAffine(src, 20, Translate(1.0, 600.0, 900.0)); // beyond the bottom-right corner
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(MaxDiff(PixelAt(out, 0, 0), PixelAt(src, 49, 39)), 0);
  EXPECT_EQ(MaxDiff(PixelAt(out, 19, 19), PixelAt(src, 49, 39)), 0);
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

// A turned crop's corners fall off the picture: they repeat edge colours, never the new surface's blank (black).
TEST(SampleAffineTest, ATurnedCropHasNoBlankCorners) {
  SDL_Surface* src = Quadrants(100, 100);
  const double t = 0.5; // about 29 degrees
  ImageAffine a;
  a.xu = std::cos(t);
  a.xv = std::sin(t);
  a.yu = -std::sin(t);
  a.yv = std::cos(t);
  a.x0 = 50.0 - (a.xu + a.xv) * 50.0;
  a.y0 = 50.0 - (a.yu + a.yv) * 50.0;
  SDL_Surface* out = SampleAffine(src, 100, a);
  ASSERT_NE(out, nullptr);
  for (int y = 0; y < 100; ++y) {
    for (int x = 0; x < 100; ++x) {
      const Rgb c = PixelAt(out, x, y);
      ASSERT_GT(c.r + c.g + c.b, 100) << x << "," << y;
    }
  }
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

TEST(SampleAffineTest, BetweenTwoPixelsTheValueIsInterpolated) {
  SDL_Surface* src = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGB24);
  auto* p = static_cast<uint8_t*>(src->pixels);
  p[0] = p[1] = p[2] = 0;
  p[3] = p[4] = p[5] = 200;
  // Output pixel 0's centre (0.5, 0.5) -> source (1.0, 0.5): exactly between the two pixel centres.
  SDL_Surface* out = SampleAffine(src, 1, Translate(1.0, 0.5, 0.0));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(PixelAt(out, 0, 0).r, 100);
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

TEST(SampleAffineTest, AlphaIsKeptOnlyWhenTheSourceHasIt) {
  SDL_Surface* opaque = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_BGR24);
  SDL_FillSurfaceRect(opaque, nullptr, SDL_MapSurfaceRGB(opaque, 10, 120, 230));
  SDL_Surface* out = SampleAffine(opaque, 4, Translate(2.0, 0.0, 0.0));
  ASSERT_NE(out, nullptr);
  EXPECT_FALSE(SurfaceHasAlpha(out)) << "a JPEG source stays a JPEG upload";
  EXPECT_EQ(MaxDiff(PixelAt(out, 1, 1), {10, 120, 230}), 0) << "channel order read right from BGR24";
  SDL_DestroySurface(out);
  SDL_DestroySurface(opaque);

  SDL_Surface* clear = SDL_CreateSurface(8, 8, SDL_PIXELFORMAT_ARGB8888);
  SDL_FillSurfaceRect(clear, nullptr, SDL_MapSurfaceRGBA(clear, 200, 50, 25, 77));
  out = SampleAffine(clear, 4, Translate(2.0, 0.0, 0.0));
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(SurfaceHasAlpha(out));
  uint8_t r = 0, g = 0, b = 0, alpha = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(out, 2, 2, &r, &g, &b, &alpha));
  EXPECT_EQ(r, 200);
  EXPECT_EQ(g, 50);
  EXPECT_EQ(b, 25);
  EXPECT_EQ(alpha, 77);
  SDL_DestroySurface(out);
  SDL_DestroySurface(clear);
}

// Straight RGBA: a clear pixel's hidden colour (black here) must not darken the edge between it and an opaque one.
TEST(SampleAffineTest, ATransparentNeighbourDoesNotFringeTheEdgeWithItsHiddenColour) {
  SDL_Surface* src = SDL_CreateSurface(2, 1, SDL_PIXELFORMAT_RGBA32);
  auto* p = static_cast<uint8_t*>(src->pixels);
  p[0] = p[1] = p[2] = p[3] = 0;   // transparent black
  p[4] = p[5] = p[6] = p[7] = 255; // opaque white
  // Output pixel 0 reads exactly between the two source pixel centres.
  SDL_Surface* out = SampleAffine(src, 1, Translate(1.0, 0.5, 0.0));
  ASSERT_NE(out, nullptr);
  uint8_t r = 0, g = 0, b = 0, a = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(out, 0, 0, &r, &g, &b, &a));
  EXPECT_EQ(r, 255) << "white stays white: no dark fringe";
  EXPECT_EQ(g, 255);
  EXPECT_EQ(b, 255);
  EXPECT_NEAR(a, 128, 1) << "half covered";
  SDL_DestroySurface(out);

  // Only clear pixels under the sample: transparent, whatever colour they hide.
  p[4] = 9;
  p[5] = 9;
  p[6] = 9;
  p[7] = 0;
  p[0] = 200;
  out = SampleAffine(src, 1, Translate(1.0, 0.5, 0.0));
  ASSERT_NE(out, nullptr);
  ASSERT_TRUE(SDL_ReadSurfacePixel(out, 0, 0, &r, &g, &b, &a));
  EXPECT_EQ(a, 0);
  SDL_DestroySurface(out);
  SDL_DestroySurface(src);
}

// An opaque RGBA source gives exactly the bytes the RGB24 path gives for the same pixels.
TEST(SampleAffineTest, AnOpaqueRgbaSourceMatchesTheRgb24Path) {
  SDL_Surface* rgb = Pattern(60, 40);
  SDL_Surface* rgba = SDL_ConvertSurface(rgb, SDL_PIXELFORMAT_RGBA32);
  ASSERT_NE(rgba, nullptr);
  ImageAffine a;
  a.xu = 0.6;
  a.xv = 0.3;
  a.yu = -0.3;
  a.yv = 0.6;
  a.x0 = 12.3;
  a.y0 = 17.7;
  SDL_Surface* want = SampleAffine(rgb, 30, a);
  SDL_Surface* got = SampleAffine(rgba, 30, a);
  ASSERT_NE(want, nullptr);
  ASSERT_NE(got, nullptr);
  for (int y = 0; y < 30; ++y) {
    for (int x = 0; x < 30; ++x) {
      uint8_t w[4] = {}, g[4] = {};
      ASSERT_TRUE(SDL_ReadSurfacePixel(want, x, y, &w[0], &w[1], &w[2], &w[3]));
      ASSERT_TRUE(SDL_ReadSurfacePixel(got, x, y, &g[0], &g[1], &g[2], &g[3]));
      ASSERT_EQ(std::memcmp(w, g, 3), 0) << x << "," << y;
      ASSERT_EQ(g[3], 255);
    }
  }
  SDL_DestroySurface(want);
  SDL_DestroySurface(got);
  SDL_DestroySurface(rgba);
  SDL_DestroySurface(rgb);
}

TEST(SampleAffineTest, BadArgumentsGiveNoSurface) {
  SDL_Surface* src = Pattern(4, 4);
  EXPECT_EQ(SampleAffine(nullptr, 4, ImageAffine{}), nullptr);
  EXPECT_EQ(SampleAffine(src, 0, ImageAffine{}), nullptr);
  ImageAffine nan;
  nan.x0 = std::nan("");
  EXPECT_EQ(SampleAffine(src, 4, nan), nullptr);
  SDL_DestroySurface(src);
}

} // namespace
} // namespace pbr
