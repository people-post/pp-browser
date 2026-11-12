#include "foundation/platform/ImageEncode.h"
#include "foundation/platform/ImageThumbnail.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {
namespace {

// The real platform thumbnailer (ImageIO on Apple, SDL_image elsewhere) on a real JPEG held in memory.
class ImageThumbnailTest : public ::testing::Test {
protected:
  static std::vector<uint8_t> Jpeg(int w, int h) {
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
    EXPECT_NE(s, nullptr);
    for (int y = 0; y < h; ++y) {
      auto* row = static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
      for (int x = 0; x < w; ++x) {
        row[x * 3 + 0] = static_cast<uint8_t>(x * 255 / w);
        row[x * 3 + 1] = static_cast<uint8_t>(y * 255 / h);
        row[x * 3 + 2] = 90;
      }
    }
    auto jpeg = EncodeJpegBytes(s, 85);
    SDL_DestroySurface(s);
    EXPECT_TRUE(jpeg);
    return jpeg ? *jpeg : std::vector<uint8_t>{};
  }
};

TEST_F(ImageThumbnailTest, AWidePictureComesBackAsASmallJpegOfTheSameShape) {
  auto thumb = MakeImageThumbnailJpeg(Jpeg(2400, 1600), 480);
  ASSERT_TRUE(thumb) << thumb.error().message;
  ASSERT_GT(thumb->size(), 3u);
  EXPECT_EQ((*thumb)[0], 0xFF);
  EXPECT_EQ((*thumb)[1], 0xD8); // a JPEG

  SDL_IOStream* io = SDL_IOFromConstMem(thumb->data(), thumb->size());
  SDL_Surface* decoded = IMG_Load_IO(io, true);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->w, 480);
  EXPECT_NEAR(decoded->h, 320, 2);
  SDL_DestroySurface(decoded);
}

TEST_F(ImageThumbnailTest, ASmallPictureIsNotBlownUp) {
  auto thumb = MakeImageThumbnailJpeg(Jpeg(200, 100), 480);
  ASSERT_TRUE(thumb) << thumb.error().message;
  SDL_IOStream* io = SDL_IOFromConstMem(thumb->data(), thumb->size());
  SDL_Surface* decoded = IMG_Load_IO(io, true);
  ASSERT_NE(decoded, nullptr);
  EXPECT_LE(decoded->w, 200);
  SDL_DestroySurface(decoded);
}

/** A `w` x `h` RGBA picture: its left half fully transparent (black under the alpha), its right half opaque red. */
SDL_Surface* HalfTransparent(const int w, const int h) {
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
  EXPECT_NE(s, nullptr);
  for (int y = 0; y < h; ++y) {
    auto* row = static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
    for (int x = 0; x < w; ++x) {
      const bool opaque = x >= w / 2;
      row[x * 4 + 0] = opaque ? 220 : 0;
      row[x * 4 + 1] = opaque ? 30 : 0;
      row[x * 4 + 2] = opaque ? 30 : 0;
      row[x * 4 + 3] = opaque ? 255 : 0;
    }
  }
  return s;
}

void ExpectPixelNear(SDL_Surface* s, const int x, const int y, const int r, const int g, const int b,
                     const char* what) {
  Uint8 pr = 0;
  Uint8 pg = 0;
  Uint8 pb = 0;
  Uint8 pa = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(s, x, y, &pr, &pg, &pb, &pa));
  EXPECT_NEAR(pr, r, 24) << what;
  EXPECT_NEAR(pg, g, 24) << what;
  EXPECT_NEAR(pb, b, 24) << what;
}

// A PNG with transparency becomes a JPEG (no alpha) preview: what was transparent is white, as the chat's light
// bubble behind it, not black.
TEST_F(ImageThumbnailTest, ATransparentPictureComesBackOnWhite) {
  SDL_Surface* source = HalfTransparent(1000, 500);
  auto png = EncodePngBytes(source);
  SDL_DestroySurface(source);
  ASSERT_TRUE(png) << png.error().message;
  auto thumb = MakeImageThumbnailJpeg(*png, 640);
  ASSERT_TRUE(thumb) << thumb.error().message;
  SDL_IOStream* io = SDL_IOFromConstMem(thumb->data(), thumb->size());
  SDL_Surface* decoded = IMG_Load_IO(io, true);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(decoded->w, 640);
  ExpectPixelNear(decoded, 40, decoded->h / 2, 255, 255, 255, "the transparent half");
  ExpectPixelNear(decoded, decoded->w - 40, decoded->h / 2, 220, 30, 30, "the opaque half");
  SDL_DestroySurface(decoded);
}

// The helper the SDL thumbnailer (Android, Windows, Linux) uses before encoding: the picture laid on white.
TEST_F(ImageThumbnailTest, FlattenOntoWhiteLaysAPictureWithAlphaOnWhite) {
  SDL_Surface* flat = FlattenOntoWhite(HalfTransparent(64, 32));
  ASSERT_NE(flat, nullptr);
  EXPECT_FALSE(SurfaceHasAlpha(flat));
  EXPECT_EQ(flat->w, 64);
  EXPECT_EQ(flat->h, 32);
  ExpectPixelNear(flat, 4, 16, 255, 255, 255, "transparent: white");
  ExpectPixelNear(flat, 60, 16, 220, 30, 30, "opaque: as it was");
  SDL_DestroySurface(flat);
}

TEST_F(ImageThumbnailTest, NotAPictureFails) {
  const std::string junk = "not an image";
  EXPECT_FALSE(MakeImageThumbnailJpeg(std::vector<uint8_t>(junk.begin(), junk.end()), 480));
  EXPECT_FALSE(MakeImageThumbnailJpeg({}, 480));
}

} // namespace
} // namespace pbr
