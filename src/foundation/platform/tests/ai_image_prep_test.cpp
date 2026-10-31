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
    // One directory per test: ctest runs each test as its own process, in parallel under -j, and a
    // shared directory lets one test's TearDown delete another's images.
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp_ai_image_prep_test-") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
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

// A JPEG whose left half is red and right half blue, with an EXIF orientation segment inserted.
std::string SaveTwoToneJpegWithOrientation(const std::filesystem::path& dir, const int orientation) {
  SDL_Surface* s = SDL_CreateSurface(200, 100, SDL_PIXELFORMAT_RGB24);
  SDL_Rect left{0, 0, 100, 100};
  SDL_Rect right{100, 0, 100, 100};
  SDL_FillSurfaceRect(s, &left, SDL_MapSurfaceRGB(s, 255, 0, 0));
  SDL_FillSurfaceRect(s, &right, SDL_MapSurfaceRGB(s, 0, 0, 255));
  const std::string path = (dir / "two_tone.jpg").string();
  EXPECT_TRUE(IMG_SaveJPG(s, path.c_str(), 95)) << SDL_GetError();
  SDL_DestroySurface(s);

  std::vector<uint8_t> jpeg;
  {
    std::ifstream in(path, std::ios::binary);
    jpeg.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const std::vector<uint8_t> segment = {0xFF, 0xE1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
                                        'M',  'M',  0,    0x2A, 0,   0,   0,   8,
                                        0,    1,    0x01, 0x12, 0,   3,   0,   0,   0, 1,
                                        0,    static_cast<uint8_t>(orientation), 0, 0,
                                        0,    0,    0,    0};
  jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  return path;
}

SDL_Surface* Decode(const std::vector<uint8_t>& bytes) {
  return IMG_Load_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
}

void ExpectRedAndBlueAt(SDL_Surface* surface, const int rx, const int ry, const int bx, const int by) {
  uint8_t r = 0, g = 0, b = 0, a = 0;
  ASSERT_TRUE(SDL_ReadSurfacePixel(surface, rx, ry, &r, &g, &b, &a));
  EXPECT_GT(r, 200) << "red expected at " << rx << "," << ry;
  EXPECT_LT(b, 60);
  ASSERT_TRUE(SDL_ReadSurfacePixel(surface, bx, by, &r, &g, &b, &a));
  EXPECT_GT(b, 200) << "blue expected at " << bx << "," << by;
  EXPECT_LT(r, 60);
}

TEST_F(AiImagePrepTest, ExifOrientationIsAppliedOnlyWhenAsked) {
  const std::string path = SaveTwoToneJpegWithOrientation(dir_, 6); // needs a quarter turn clockwise

  auto untouched = PrepareAiImageFromFile(path);
  ASSERT_TRUE(untouched) << untouched.error().message;
  EXPECT_EQ(untouched->width, 200) << "the AI path is unchanged";
  EXPECT_EQ(untouched->height, 100);

  ImagePrepOptions options;
  options.apply_exif_orientation = true;
  auto upright = PrepareAiImageFromFile(path, 1536u * 1024u, 2048, options);
  ASSERT_TRUE(upright) << upright.error().message;
  EXPECT_EQ(upright->width, 100);
  EXPECT_EQ(upright->height, 200);
  SDL_Surface* surface = Decode(upright->bytes);
  ASSERT_NE(surface, nullptr);
  ExpectRedAndBlueAt(surface, 50, 25, 50, 175); // the left half is now the top
  SDL_DestroySurface(surface);
}

TEST_F(AiImagePrepTest, EveryOrientationPutsTheRedHalfWhereItBelongs) {
  // For each tag: where the red (source left) half ends up, as a point of the upright result.
  struct Case {
    int orientation;
    int width, height;
    int rx, ry, bx, by;
  };
  const Case cases[] = {
      {2, 200, 100, 150, 50, 50, 50},   // mirrored: red on the right
      {3, 200, 100, 150, 50, 50, 50},   // 180 degrees: red on the right
      {4, 200, 100, 50, 50, 150, 50},   // vertical flip: red stays left
      {5, 100, 200, 50, 50, 50, 150},   // transpose: red on top
      {6, 100, 200, 50, 50, 50, 150},   // 90 clockwise: red on top
      {7, 100, 200, 50, 150, 50, 50},   // transverse: red at the bottom
      {8, 100, 200, 50, 150, 50, 50},   // 90 counter-clockwise: red at the bottom
  };
  for (const Case& c : cases) {
    const std::string path = SaveTwoToneJpegWithOrientation(dir_, c.orientation);
    ImagePrepOptions options;
    options.apply_exif_orientation = true;
    auto result = PrepareAiImageFromFile(path, 1536u * 1024u, 2048, options);
    ASSERT_TRUE(result) << c.orientation;
    EXPECT_EQ(result->width, c.width) << c.orientation;
    EXPECT_EQ(result->height, c.height) << c.orientation;
    SDL_Surface* surface = Decode(result->bytes);
    ASSERT_NE(surface, nullptr);
    SCOPED_TRACE(c.orientation);
    ExpectRedAndBlueAt(surface, c.rx, c.ry, c.bx, c.by);
    SDL_DestroySurface(surface);
  }
}

TEST_F(AiImagePrepTest, PixelLimitRefusesBeforeDecoding) {
  ImagePrepOptions options;
  options.max_pixels = 50'000;

  const std::string big = Save(MakeNoisySurface(300, 300, false), "big.png"); // 90 000 pixels
  auto refused = PrepareAiImageFromFile(big, 4'000'000, 2048, options);
  ASSERT_FALSE(refused);
  EXPECT_EQ(AiImagePrepErrorOf(refused.error()), AiImagePrepError::TooLarge);

  const std::string small = Save(MakeNoisySurface(100, 100, false), "fits.png");
  EXPECT_TRUE(PrepareAiImageFromFile(small, 4'000'000, 2048, options));

  // A small file whose header claims a gigantic picture: refused on the header alone (a decode would
  // try to allocate it).
  std::vector<uint8_t> bomb = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                               0x00, 0x01, 0x86, 0xA0, 0x00, 0x01, 0x86, 0xA0, 8, 2, 0, 0, 0};
  const std::string bomb_path = (dir_ / "bomb.png").string();
  {
    std::ofstream out(bomb_path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bomb.data()), static_cast<std::streamsize>(bomb.size()));
  }
  auto bomb_result = PrepareAiImageFromFile(bomb_path, 4'000'000, 2048, options);
  ASSERT_FALSE(bomb_result);
  EXPECT_EQ(AiImagePrepErrorOf(bomb_result.error()), AiImagePrepError::TooLarge);
}

TEST_F(AiImagePrepTest, PixelLimitTreatsAnUnreadableHeadAsNotAnImage) {
  ImagePrepOptions options;
  options.max_pixels = 1'000'000;
  const std::string path = (dir_ / "text.png").string();
  std::ofstream(path) << "not an image at all";
  auto result = PrepareAiImageFromFile(path, 4'000'000, 2048, options);
  ASSERT_FALSE(result);
  EXPECT_EQ(AiImagePrepErrorOf(result.error()), AiImagePrepError::NotAnImage);
}

} // namespace
} // namespace pbr
