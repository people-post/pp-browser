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
  EXPECT_EQ((*thumb)[1], 0xD8);  // a JPEG

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

TEST_F(ImageThumbnailTest, NotAPictureFails) {
  const std::string junk = "not an image";
  EXPECT_FALSE(MakeImageThumbnailJpeg(std::vector<uint8_t>(junk.begin(), junk.end()), 480));
  EXPECT_FALSE(MakeImageThumbnailJpeg({}, 480));
}

} // namespace
} // namespace pbr
