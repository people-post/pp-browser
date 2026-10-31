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

std::vector<uint8_t> Segment(const uint8_t marker, const std::string& body) {
  const uint16_t len = static_cast<uint16_t>(body.size() + 2);
  std::vector<uint8_t> seg = {0xFF, marker, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
  seg.insert(seg.end(), body.begin(), body.end());
  return seg;
}

bool Decodes(const std::vector<uint8_t>& bytes, const int expected_width) {
  SDL_Surface* surface = IMG_Load_IO(SDL_IOFromConstMem(bytes.data(), bytes.size()), true);
  if (!surface) {
    return false;
  }
  const bool ok = surface->w == expected_width;
  SDL_DestroySurface(surface);
  return ok;
}

TEST_F(ImageMetadataTest, StripRemovesGpsButKeepsAViewableUprightPicture) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(64, 48, "gps.jpg"));
  const auto exif = ExifSegmentWithGps(6);
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  ASSERT_TRUE(Contains(jpeg, "GPSLATITUDE"));

  const auto stripped = StripJpegLocation(jpeg);
  ASSERT_TRUE(stripped);
  EXPECT_FALSE(Contains(*stripped, "GPSLATITUDE"));
  EXPECT_FALSE(Contains(*stripped, "-121.4737E"));
  // The orientation survives as a minimal EXIF segment that holds nothing else.
  const auto info = ReadImageHeaderBytes(*stripped);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->exif_orientation, 6);
  EXPECT_EQ(info->width, 64);
  EXPECT_EQ(info->height, 48);
  EXPECT_TRUE(Decodes(*stripped, 64));
}

TEST_F(ImageMetadataTest, StripOfAnUprightPictureLeavesNoExifAtAll) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(40, 40, "up.jpg"));
  const auto exif = ExifSegmentWithGps(1);
  jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
  const auto stripped = StripJpegLocation(jpeg);
  ASSERT_TRUE(stripped);
  EXPECT_FALSE(Contains(*stripped, "Exif"));
  EXPECT_FALSE(Contains(*stripped, "GPSLATITUDE"));
  EXPECT_TRUE(Decodes(*stripped, 40));
}

TEST_F(ImageMetadataTest, StripDropsCommentsXmpIptcAndVendorSegments) {
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(40, 40, "c.jpg"));
  std::vector<uint8_t> extra;
  for (const auto& seg : {Segment(0xFE, "taken at home"), Segment(0xE1, "http://ns.adobe.com/xap/1.0/ XMP-GPS-HOME"),
                          Segment(0xED, "Photoshop 3.0 IPTC-CITY"), Segment(0xEB, "VENDOR-LOCATION"),
                          Segment(0xE5, "MAKERNOTE-SERIAL")}) {
    extra.insert(extra.end(), seg.begin(), seg.end());
  }
  jpeg.insert(jpeg.begin() + 2, extra.begin(), extra.end());
  for (const char* needle : {"taken at home", "XMP-GPS-HOME", "IPTC-CITY", "VENDOR-LOCATION", "MAKERNOTE-SERIAL"}) {
    ASSERT_TRUE(Contains(jpeg, needle)) << needle;
  }
  const auto stripped = StripJpegLocation(jpeg);
  ASSERT_TRUE(stripped);
  for (const char* needle : {"taken at home", "XMP-GPS-HOME", "IPTC-CITY", "VENDOR-LOCATION", "MAKERNOTE-SERIAL"}) {
    EXPECT_FALSE(Contains(*stripped, needle)) << needle;
  }
  EXPECT_TRUE(Decodes(*stripped, 40));
}

TEST_F(ImageMetadataTest, StripDropsMpfSecondPictureTrailerAndJfxxThumbnail) {
  const auto base = ReadFile(SaveJpeg(40, 40, "mpf.jpg"));
  const auto second = ReadFile(SaveJpeg(16, 16, "second.jpg"));
  std::vector<uint8_t> jpeg = base;
  // APP0 JFXX thumbnail and APP2 MPF right after SOI (any further APP0 JFIF of the encoder stays as it was).
  std::vector<uint8_t> extra;
  for (const auto& seg : {Segment(0xE0, std::string("JFXX\0\x10", 6) + "THUMBNAIL-LOCATION"),
                          Segment(0xE2, std::string("MPF\0", 4) + "MPF-INDEX")}) {
    extra.insert(extra.end(), seg.begin(), seg.end());
  }
  jpeg.insert(jpeg.begin() + 2, extra.begin(), extra.end());
  // A second complete picture and a trailer after the first end of image.
  jpeg.insert(jpeg.end(), second.begin(), second.end());
  const std::string trailer = "TRAILER-GPS-DATA";
  jpeg.insert(jpeg.end(), trailer.begin(), trailer.end());

  const auto stripped = StripJpegLocation(jpeg);
  ASSERT_TRUE(stripped);
  EXPECT_FALSE(Contains(*stripped, "THUMBNAIL-LOCATION"));
  EXPECT_FALSE(Contains(*stripped, "MPF-INDEX"));
  EXPECT_FALSE(Contains(*stripped, "TRAILER-GPS-DATA"));
  EXPECT_LT(stripped->size(), base.size() + 1) << "nothing of the second picture or the trailer is left";
  EXPECT_EQ((*stripped)[stripped->size() - 2], 0xFF);
  EXPECT_EQ((*stripped)[stripped->size() - 1], 0xD9) << "ends at the first end of image";
  EXPECT_TRUE(Decodes(*stripped, 40));
}

TEST_F(ImageMetadataTest, StripEmptiesAJfifThumbnail) {
  // JFIF with a 2x2 thumbnail of recognisable bytes.
  std::string jfif("JFIF\0\x01\x01\x00\x00\x01\x00\x01\x02\x02", 14);
  jfif += "THUMB-PIXELS";
  std::vector<uint8_t> jpeg = ReadFile(SaveJpeg(32, 32, "jfif.jpg"));
  const auto seg = Segment(0xE0, jfif);
  jpeg.insert(jpeg.begin() + 2, seg.begin(), seg.end());
  const auto stripped = StripJpegLocation(jpeg);
  ASSERT_TRUE(stripped);
  EXPECT_FALSE(Contains(*stripped, "THUMB-PIXELS"));
  EXPECT_TRUE(Decodes(*stripped, 32));
}

TEST_F(ImageMetadataTest, StripFailsClosedOnAnythingMalformed) {
  EXPECT_FALSE(StripJpegLocation({}));
  EXPECT_FALSE(StripJpegLocation({'n', 'o', 't', ' ', 'a', ' ', 'j', 'p', 'g'}));
  const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 1, 2, 3};
  EXPECT_FALSE(StripJpegLocation(png));
  const std::vector<uint8_t> truncated_segment = {0xFF, 0xD8, 0xFF, 0xE1, 0xFF, 0xFF, 1, 2};
  EXPECT_FALSE(StripJpegLocation(truncated_segment)) << "a segment that runs past the end";

  const auto good = ReadFile(SaveJpeg(40, 40, "cut.jpg"));
  ASSERT_TRUE(StripJpegLocation(good));
  std::vector<uint8_t> no_end(good.begin(), good.end() - 2); // end-of-image marker cut off
  EXPECT_FALSE(StripJpegLocation(no_end));
  std::vector<uint8_t> half(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
  EXPECT_FALSE(StripJpegLocation(half));
  std::vector<uint8_t> garbage_between = good;
  garbage_between.insert(garbage_between.begin() + 2, {0x12, 0x34, 0x56, 0x78});
  EXPECT_FALSE(StripJpegLocation(garbage_between)) << "bytes where a marker must be";
}

TEST_F(ImageMetadataTest, KindIsDecidedByTheFirstBytesNotTheName) {
  const std::string jpeg = SaveJpeg(20, 20, "noext_source.jpg");
  const std::string renamed = (dir_ / "IMG_0001").string(); // no extension at all
  std::filesystem::copy_file(jpeg, renamed);
  EXPECT_EQ(ImageKindOfFile(renamed), ImageKind::Jpeg);
  EXPECT_EQ(ImageKindOfFile(SavePng(8, 8, "x.dat")), ImageKind::Png) << "a PNG named .dat";
  EXPECT_EQ(ImageKindOfBytes({'G', 'I', 'F', '8', '9', 'a', 1}), ImageKind::Gif);
  EXPECT_EQ(ImageKindOfBytes({'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'}), ImageKind::Webp);
  EXPECT_EQ(ImageKindOfBytes({'h', 'e', 'l', 'l', 'o'}), ImageKind::Unknown);
  EXPECT_EQ(ImageKindOfFile((dir_ / "missing").string()), ImageKind::Unknown);
}

TEST_F(ImageMetadataTest, PixelCapReadsTheDeclaredSizeOnly) {
  EXPECT_TRUE(ImageWithinPixelCap(SaveJpeg(100, 100, "ok.jpg"), 10'000));
  EXPECT_FALSE(ImageWithinPixelCap(SaveJpeg(100, 100, "big.jpg"), 9'999));
  // A tiny file whose header claims 30000 x 30000.
  std::vector<uint8_t> bomb = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                               0x00, 0x00, 0x75, 0x30, 0x00, 0x00, 0x75, 0x30, 8, 2, 0, 0, 0};
  const std::string path = (dir_ / "claims_huge.png").string();
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bomb.data()), static_cast<std::streamsize>(bomb.size()));
  }
  EXPECT_FALSE(ImageWithinPixelCap(path, 64'000'000));
  // Unknown format or a damaged head is not displayed inline either.
  const std::string text = (dir_ / "text.png").string();
  std::ofstream(text) << "not a picture";
  EXPECT_FALSE(ImageWithinPixelCap(text, 64'000'000));
  EXPECT_FALSE(ImageWithinPixelCap((dir_ / "missing.png").string(), 64'000'000));
}

} // namespace
} // namespace pbr
