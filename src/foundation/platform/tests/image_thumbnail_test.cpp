#include "foundation/platform/ImageEncode.h"
#include "foundation/platform/ImageThumbnail.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

// The real platform thumbnailer (ImageIO on Apple, SDL_image elsewhere) on a real JPEG.
class ImageThumbnailTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_image_thumb_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::string WriteJpeg(int w, int h) {
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
    const auto path = dir_ / "big.jpg";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(jpeg->data()),
                                                 static_cast<std::streamsize>(jpeg->size()));
    return path.string();
  }

  std::filesystem::path dir_;
};

TEST_F(ImageThumbnailTest, AWidePictureComesBackAsASmallJpegOfTheSameShape) {
  const std::string path = WriteJpeg(2400, 1600);
  auto thumb = MakeImageThumbnailJpeg(path, 480);
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
  const std::string path = WriteJpeg(200, 100);
  auto thumb = MakeImageThumbnailJpeg(path, 480);
  ASSERT_TRUE(thumb) << thumb.error().message;
  SDL_IOStream* io = SDL_IOFromConstMem(thumb->data(), thumb->size());
  SDL_Surface* decoded = IMG_Load_IO(io, true);
  ASSERT_NE(decoded, nullptr);
  EXPECT_LE(decoded->w, 200);
  SDL_DestroySurface(decoded);
}

TEST_F(ImageThumbnailTest, NotAPictureFails) {
  const auto path = dir_ / "junk.jpg";
  std::ofstream(path) << "not an image";
  EXPECT_FALSE(MakeImageThumbnailJpeg(path.string(), 480));
}

} // namespace
} // namespace pbr
