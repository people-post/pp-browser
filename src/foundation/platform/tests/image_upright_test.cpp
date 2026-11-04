// A picture shown in the app (a chat bubble, the full picture preview) is decoded by the UI's texture loader through
// DecodeImageUpright: a phone's portrait photo, stored sideways with an EXIF orientation, must stand up (Kenneth
// 2026-11-04, PLAN-ui-refresh-impl 62.3: "这张图就没有正过来").
#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {
namespace {

// A 200 x 100 JPEG, left half red and right half blue, with an EXIF segment holding `orientation` (0: none).
std::vector<uint8_t> TwoToneJpeg(const int orientation) {
  SDL_Surface* s = SDL_CreateSurface(200, 100, SDL_PIXELFORMAT_RGB24);
  SDL_Rect left{0, 0, 100, 100};
  SDL_Rect right{100, 0, 100, 100};
  SDL_FillSurfaceRect(s, &left, SDL_MapSurfaceRGB(s, 255, 0, 0));
  SDL_FillSurfaceRect(s, &right, SDL_MapSurfaceRGB(s, 0, 0, 255));
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  EXPECT_TRUE(IMG_SaveJPG_IO(s, io, false, 95)) << SDL_GetError();
  SDL_DestroySurface(s);
  std::vector<uint8_t> jpeg(static_cast<size_t>(SDL_GetIOSize(io)));
  SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
  SDL_ReadIO(io, jpeg.data(), jpeg.size());
  SDL_CloseIO(io);
  if (orientation > 0) {
    const std::vector<uint8_t> segment = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
                                          'M',  'M',  0,    0x2A, 0,   0,   0,   8,
                                          0,    1,    0x01, 0x12, 0,   3,   0,   0,   0, 1,
                                          0,    static_cast<uint8_t>(orientation), 0, 0,
                                          0,    0,    0,    0};
    jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
  }
  return jpeg;
}

void ExpectColourAt(SDL_Surface* surface, const int x, const int y, const bool red) {
  uint8_t r = 0, g = 0, b = 0, a = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a));
  if (red) {
    EXPECT_GT(r, 200) << "red expected at " << x << "," << y;
    EXPECT_LT(b, 60);
  } else {
    EXPECT_GT(b, 200) << "blue expected at " << x << "," << y;
    EXPECT_LT(r, 60);
  }
}

TEST(ImageUprightTest, APhotoTaggedToTurnAQuarterStandsUp) {
  const std::vector<uint8_t> jpeg = TwoToneJpeg(6);
  SDL_Surface* surface = DecodeImageUpright(jpeg.data(), jpeg.size(), "jpg");
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->w, 100) << "portrait, not on its side";
  EXPECT_EQ(surface->h, 200);
  ExpectColourAt(surface, 50, 25, true);  // the stored left half is now the top
  ExpectColourAt(surface, 50, 175, false);
  SDL_DestroySurface(surface);
}

TEST(ImageUprightTest, AnUprightPhotoAndOneWithoutATagStayAsTheyAre) {
  for (const int orientation : {0, 1}) {
    const std::vector<uint8_t> jpeg = TwoToneJpeg(orientation);
    SDL_Surface* surface = DecodeImageUpright(jpeg.data(), jpeg.size(), "jpg");
    ASSERT_NE(surface, nullptr) << orientation;
    EXPECT_EQ(surface->w, 200) << orientation;
    EXPECT_EQ(surface->h, 100) << orientation;
    ExpectColourAt(surface, 50, 50, true);
    ExpectColourAt(surface, 150, 50, false);
    SDL_DestroySurface(surface);
  }
}

TEST(ImageUprightTest, AWrongExtensionDoesNotMatterAndGarbageIsNoPicture) {
  const std::vector<uint8_t> jpeg = TwoToneJpeg(6);
  SDL_Surface* surface = DecodeImageUpright(jpeg.data(), jpeg.size(), "png");
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(surface->w, 100);
  SDL_DestroySurface(surface);
  const std::vector<uint8_t> junk(64, 0x5A);
  EXPECT_EQ(DecodeImageUpright(junk.data(), junk.size(), "jpg"), nullptr);
  EXPECT_EQ(DecodeImageUpright(nullptr, 0, "jpg"), nullptr);
}

} // namespace
} // namespace pbr
