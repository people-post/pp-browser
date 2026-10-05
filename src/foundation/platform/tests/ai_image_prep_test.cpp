#include "foundation/platform/AiImagePrep.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace pbr {
namespace {

// Deterministic noise over a gradient so JPEG cannot shrink it to nothing.
SDL_Surface* MakeNoisySurface(int w, int h, bool alpha) {
  SDL_Surface* s = SDL_CreateSurface(w, h, alpha ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_RGB24);
  EXPECT_NE(s, nullptr);
  uint32_t state = 12345;
  const int bpp = SDL_BYTESPERPIXEL(s->format);
  for (int y = 0; y < h; ++y) {
    auto* row = static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
    for (int x = 0; x < w; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x * bpp + 0] = static_cast<uint8_t>((x * 255 / w) ^ ((state >> 24) & 0x3F));
      row[x * bpp + 1] = static_cast<uint8_t>((y * 255 / h) ^ ((state >> 16) & 0x3F));
      row[x * bpp + 2] = static_cast<uint8_t>((state >> 8) & 0xFF);
      if (alpha) {
        row[x * bpp + 3] = static_cast<uint8_t>(x < w / 2 ? 128 : 255);
      }
    }
  }
  return s;
}

class AiImagePrepTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / "pp_ai_image_prep_test";
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string Save(SDL_Surface* s, const std::string& name) {
    const std::string path = (dir_ / name).string();
    const bool png = name.size() > 4 && name.substr(name.size() - 4) == ".png";
    const bool ok = png ? IMG_SavePNG(s, path.c_str()) : IMG_SaveJPG(s, path.c_str(), 90);
    EXPECT_TRUE(ok) << SDL_GetError();
    SDL_DestroySurface(s);
    return path;
  }

  std::filesystem::path dir_;
};

bool HasSubstring(const std::vector<uint8_t>& bytes, const std::string& needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

TEST_F(AiImagePrepTest, SmallImageKeepsDimensionsAndFits) {
  const std::string path = Save(MakeNoisySurface(200, 100, false), "small.jpg");
  auto result = PrepareAiImageFromFile(path);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->mime, "image/jpeg");
  EXPECT_EQ(result->width, 200);
  EXPECT_EQ(result->height, 100);
  EXPECT_LE(result->bytes.size(), 1536u * 1024u);
  EXPECT_EQ(result->bytes[0], 0xFF);
  EXPECT_EQ(result->bytes[1], 0xD8);
}

TEST_F(AiImagePrepTest, LargeImageIsScaledAndFits) {
  const std::string path = Save(MakeNoisySurface(5000, 3000, false), "large.png");
  auto result = PrepareAiImageFromFile(path);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_LE(std::max(result->width, result->height), 2048);
  EXPECT_LE(result->bytes.size(), 1536u * 1024u);
}

TEST_F(AiImagePrepTest, HopelesslySmallLimitIsTooLarge) {
  const std::string path = Save(MakeNoisySurface(512, 512, false), "noisy.jpg");
  auto result = PrepareAiImageFromFile(path, 100);
  ASSERT_FALSE(result);
  EXPECT_EQ(AiImagePrepErrorOf(result.error()), AiImagePrepError::TooLarge);
}

TEST_F(AiImagePrepTest, AlphaPngStaysPng) {
  const std::string path = Save(MakeNoisySurface(64, 64, true), "alpha.png");
  auto result = PrepareAiImageFromFile(path, 1536u * 1024u);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->mime, "image/png");
  EXPECT_EQ(result->bytes[1], 'P');
}

TEST_F(AiImagePrepTest, AlphaPngTooBigFallsBackToJpeg) {
  const std::string path = Save(MakeNoisySurface(256, 256, true), "alpha_big.png");
  auto result = PrepareAiImageFromFile(path, 40 * 1024u);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(result->mime, "image/jpeg");
  EXPECT_LE(result->bytes.size(), 40 * 1024u);
}

TEST_F(AiImagePrepTest, TextFileIsNotAnImage) {
  const std::string path = (dir_ / "note.txt").string();
  std::ofstream(path) << "hello, this is not an image";
  auto result = PrepareAiImageFromFile(path);
  ASSERT_FALSE(result);
  EXPECT_EQ(AiImagePrepErrorOf(result.error()), AiImagePrepError::NotAnImage);
}

TEST_F(AiImagePrepTest, EmptyAndMissingPathsAreNotAnImage) {
  auto empty = PrepareAiImageFromFile("");
  ASSERT_FALSE(empty);
  EXPECT_EQ(AiImagePrepErrorOf(empty.error()), AiImagePrepError::NotAnImage);
  auto missing = PrepareAiImageFromFile((dir_ / "nope.png").string());
  ASSERT_FALSE(missing);
  EXPECT_EQ(AiImagePrepErrorOf(missing.error()), AiImagePrepError::NotAnImage);
}

TEST_F(AiImagePrepTest, ReEncodingDropsExifSegment) {
  const std::string path = Save(MakeNoisySurface(64, 64, false), "exif.jpg");
  std::vector<uint8_t> jpeg;
  {
    std::ifstream in(path, std::ios::binary);
    jpeg.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  ASSERT_GT(jpeg.size(), 2u);
  const std::string payload("Exif\0\0GPSMARKER", 14);
  const uint16_t len = static_cast<uint16_t>(payload.size() + 2);
  std::vector<uint8_t> segment = {0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
  segment.insert(segment.end(), payload.begin(), payload.end());
  jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end()); // right after SOI
  ASSERT_TRUE(HasSubstring(jpeg, "Exif"));
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }
  auto result = PrepareAiImageFromFile(path);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_FALSE(HasSubstring(result->bytes, "Exif"));
  EXPECT_FALSE(HasSubstring(result->bytes, "GPSMARKER"));
}

} // namespace
} // namespace pbr
