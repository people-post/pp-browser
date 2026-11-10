// CropSurface: the part of a picture a profile photo (or later the crop page) keeps.
#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <climits>

namespace pbr {
namespace {

// 40 x 20: left half red, right half blue.
SDL_Surface* TwoHalves(const SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA32, const uint8_t alpha = 255) {
  SDL_Surface* s = SDL_CreateSurface(40, 20, format);
  SDL_Rect left{0, 0, 20, 20};
  SDL_Rect right{20, 0, 20, 20};
  SDL_FillSurfaceRect(s, &left, SDL_MapSurfaceRGBA(s, 255, 0, 0, alpha));
  SDL_FillSurfaceRect(s, &right, SDL_MapSurfaceRGBA(s, 0, 0, 255, alpha));
  return s;
}

void ExpectPixel(SDL_Surface* s, const int x, const int y, const uint8_t r, const uint8_t g, const uint8_t b,
                 const uint8_t a = 255) {
  uint8_t pr = 0, pg = 0, pb = 0, pa = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa));
  EXPECT_EQ(pr, r) << x << "," << y;
  EXPECT_EQ(pg, g) << x << "," << y;
  EXPECT_EQ(pb, b) << x << "," << y;
  EXPECT_EQ(pa, a) << x << "," << y;
}

TEST(CropSurfaceTest, CopiesTheRequestedPartAndLeavesTheSourceAlone) {
  SDL_Surface* source = TwoHalves();
  SDL_Surface* out = CropSurface(source, SDL_Rect{10, 0, 20, 20});
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 20);
  EXPECT_EQ(out->h, 20);
  ExpectPixel(out, 5, 10, 255, 0, 0);
  ExpectPixel(out, 15, 10, 0, 0, 255);
  EXPECT_EQ(source->w, 40) << "the source is not consumed";
  ExpectPixel(source, 5, 10, 255, 0, 0);
  SDL_DestroySurface(out);
  SDL_DestroySurface(source);
}

TEST(CropSurfaceTest, ARectangleReachingOutsideIsClampedToThePicture) {
  SDL_Surface* source = TwoHalves();
  SDL_Surface* out = CropSurface(source, SDL_Rect{-10, -5, 30, 100});
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 20);
  EXPECT_EQ(out->h, 20);
  ExpectPixel(out, 0, 0, 255, 0, 0);
  SDL_DestroySurface(out);

  out = CropSurface(source, SDL_Rect{30, 10, 1000, 1000});
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 10);
  EXPECT_EQ(out->h, 10);
  ExpectPixel(out, 0, 0, 0, 0, 255);
  SDL_DestroySurface(out);

  out = CropSurface(source, SDL_Rect{10, 0, INT_MAX, 20}); // x + w would wrap a 32-bit sum
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 30);
  SDL_DestroySurface(out);
  SDL_DestroySurface(source);
}

TEST(CropSurfaceTest, AnEmptyOrFullyOutsideRectangleIsAnError) {
  SDL_Surface* source = TwoHalves();
  for (const SDL_Rect bad : {SDL_Rect{0, 0, 0, 10}, SDL_Rect{0, 0, 10, 0}, SDL_Rect{0, 0, -4, -4},
                             SDL_Rect{40, 0, 10, 10}, SDL_Rect{0, 20, 10, 10}, SDL_Rect{-30, 0, 20, 20}}) {
    EXPECT_EQ(CropSurface(source, bad), nullptr) << bad.x << "," << bad.y << " " << bad.w << "x" << bad.h;
  }
  EXPECT_EQ(CropSurface(nullptr, SDL_Rect{0, 0, 1, 1}), nullptr);
  SDL_DestroySurface(source);
}

TEST(CropSurfaceTest, TranslucentPixelsAreCopiedNotBlendedDarker) {
  SDL_Surface* source = TwoHalves(SDL_PIXELFORMAT_RGBA32, 128);
  SDL_Surface* out = CropSurface(source, SDL_Rect{0, 0, 10, 10});
  ASSERT_NE(out, nullptr);
  ExpectPixel(out, 3, 3, 255, 0, 0, 128);
  EXPECT_TRUE(SurfaceHasAlpha(out));
  SDL_BlendMode mode = SDL_BLENDMODE_NONE;
  ASSERT_TRUE(SDL_GetSurfaceBlendMode(source, &mode));
  EXPECT_EQ(mode, SDL_BLENDMODE_BLEND) << "the source keeps its blend mode";
  SDL_DestroySurface(out);
  SDL_DestroySurface(source);
}

} // namespace
} // namespace pbr
