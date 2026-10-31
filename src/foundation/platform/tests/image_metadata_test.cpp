#include "foundation/platform/ImageMetadata.h"

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

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool Contains(const std::vector<uint8_t>& bytes, const std::string& needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

/** An Exif APP1 segment (big endian) holding the orientation and a fake GPS block that must not survive. */
std::vector<uint8_t> ExifSegmentWithGps(const int orientation) {
  std::vector<uint8_t> tiff = {'M', 'M', 0, 0x2A, 0, 0, 0, 8,
                               0,   2,                                        // two entries
                               0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, static_cast<uint8_t>(orientation), 0, 0,
                               0x88, 0x25, 0, 4, 0, 0, 0, 1, 0, 0, 0, 38, // GPS IFD pointer
                               0, 0, 0, 0};
  const std::string gps = "GPSLATITUDE-31.2304N-LONGITUDE-121.4737E";
  tiff.insert(tiff.end(), gps.begin(), gps.end());
  std::vector<uint8_t> payload = {'E', 'x', 'i', 'f', 0, 0};
  payload.insert(payload.end(), tiff.begin(), tiff.end());
  const uint16_t len = static_cast<uint16_t>(payload.size() + 2);
  std::vector<uint8_t> segment = {0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
  segment.insert(segment.end(), payload.begin(), payload.end());
  return segment;
}

class ImageMetadataTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp_image_metadata_test-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string SaveJpeg(const int w, const int h, const std::string& name) {
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
    EXPECT_NE(s, nullptr);
    SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGB(s, 200, 30, 30));
    const std::string path = (dir_ / name).string();
    EXPECT_TRUE(IMG_SaveJPG(s, path.c_str(), 90)) << SDL_GetError();
    SDL_DestroySurface(s);
    return path;
  }

  std::string SavePng(const int w, const int h, const std::string& name) {
    SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
    EXPECT_NE(s, nullptr);
    const std::string path = (dir_ / name).string();
    EXPECT_TRUE(IMG_SavePNG(s, path.c_str())) << SDL_GetError();
    SDL_DestroySurface(s);
    return path;
  }

  std::filesystem::path dir_;
};

TEST_F(ImageMetadataTest, ReadsSizeOfJpegAndPngWithoutDecoding) {
  const auto jpeg = ReadImageHeader(SaveJpeg(120, 80, "a.jpg"));
  ASSERT_TRUE(jpeg);
  EXPECT_EQ(jpeg->width, 120);
  EXPECT_EQ(jpeg->height, 80);
  EXPECT_EQ(jpeg->exif_orientation, 1);
  const auto png = ReadImageHeader(SavePng(33, 17, "a.png"));
  ASSERT_TRUE(png);
  EXPECT_EQ(png->width, 33);
  EXPECT_EQ(png->height, 17);
}

TEST_F(ImageMetadataTest, ReadsGifAndWebpHeaders) {
  const std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xC8, 0x00, 0, 0, 0};
  const auto g = ReadImageHeaderBytes(gif);
  ASSERT_TRUE(g);
  EXPECT_EQ(g->width, 320);
  EXPECT_EQ(g->height, 200);
  // Lossy WebP: RIFF, size, WEBP, "VP8 ", chunk size, frame tag (3), start code, 14-bit width / height.
  std::vector<uint8_t> webp = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' ',
                               0,   0,   0,   0,   0, 0, 0,    0x9d, 0x01, 0x2a, 0x90, 0x01, 0x2C, 0x01,
                               0,   0};
  const auto w = ReadImageHeaderBytes(webp);
  ASSERT_TRUE(w);
  EXPECT_EQ(w->width, 400);
  EXPECT_EQ(w->height, 300);
}

TEST_F(ImageMetadataTest, UnknownAndDamagedHeadsAreNullopt) {
  EXPECT_FALSE(ReadImageHeaderBytes({}));
  EXPECT_FALSE(ReadImageHeaderBytes({'B', 'M', 0, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(ReadImageHeaderBytes({0xFF, 0xD8, 0xFF, 0xE0, 0x00}));
  EXPECT_FALSE(ReadImageHeader((dir_ / "missing.jpg").string()));
}

TEST_F(ImageMetadataTest, ReadsExifOrientationBeyondALargeMetadataSegment) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(64, 48, "o.jpg"));
  const auto exif = ExifSegmentWithGps(6);
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  const auto info = ReadImageHeaderBytes(jpeg);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->exif_orientation, 6);
  EXPECT_EQ(info->width, 64);
  EXPECT_EQ(info->height, 48);
}

TEST_F(ImageMetadataTest, StripRemovesGpsButKeepsAViewableUprightPicture) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(64, 48, "gps.jpg"));
  const auto exif = ExifSegmentWithGps(6);
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  ASSERT_TRUE(Contains(jpeg, "GPSLATITUDE"));

  const auto stripped = StripJpegLocation(jpeg);
  EXPECT_FALSE(Contains(stripped, "GPSLATITUDE"));
  EXPECT_FALSE(Contains(stripped, "-121.4737E"));
  // The orientation survives as a minimal EXIF segment that holds nothing else.
  const auto info = ReadImageHeaderBytes(stripped);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->exif_orientation, 6);
  EXPECT_EQ(info->width, 64);
  EXPECT_EQ(info->height, 48);
  // Still a decodable JPEG.
  SDL_IOStream* io = SDL_IOFromConstMem(stripped.data(), stripped.size());
  SDL_Surface* surface = IMG_Load_IO(io, true);
  ASSERT_NE(surface, nullptr) << SDL_GetError();
  EXPECT_EQ(surface->w, 64);
  SDL_DestroySurface(surface);
}

TEST_F(ImageMetadataTest, StripOfAnUprightPictureLeavesNoExifAtAll) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(40, 40, "up.jpg"));
  const auto exif = ExifSegmentWithGps(1);
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  const auto stripped = StripJpegLocation(jpeg);
  EXPECT_FALSE(Contains(stripped, "Exif"));
  EXPECT_FALSE(Contains(stripped, "GPSLATITUDE"));
}

TEST_F(ImageMetadataTest, StripDropsCommentsAndLeavesNonJpegAlone) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(40, 40, "c.jpg"));
  const std::string note = "taken at home";
  std::vector<uint8_t> comment = {0xFF, 0xFE, 0, static_cast<uint8_t>(note.size() + 2)};
  comment.insert(comment.end(), note.begin(), note.end());
  jpeg.insert(jpeg.begin() + 2, comment.begin(), comment.end());
  ASSERT_TRUE(Contains(jpeg, "taken at home"));
  EXPECT_FALSE(Contains(StripJpegLocation(jpeg), "taken at home"));

  const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 1, 2, 3};
  EXPECT_EQ(StripJpegLocation(png), png);
  const std::vector<uint8_t> broken = {0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 1, 2};
  EXPECT_EQ(StripJpegLocation(broken), broken) << "a truncated segment is not touched";
}

} // namespace
} // namespace pbr
