// The profile photo that goes up: straightened by its EXIF tag, square (the middle, or the part the person framed),
// at most 512 px (never enlarged), and either way words for the person when it cannot be done (PLAN-avatar-edit, phase one).
#include "foundation/platform/ProfileIconImagePrep.h"

#include "foundation/i18n/LocalizationService.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pbr {
namespace {

struct Rgb {
  uint8_t r, g, b;
};
constexpr Rgb kRed{255, 0, 0};
constexpr Rgb kGreen{0, 255, 0};
constexpr Rgb kBlue{0, 0, 255};
constexpr Rgb kYellow{255, 255, 0};

SDL_Surface* Filled(const int w, const int h, const SDL_PixelFormat format) {
  SDL_Surface* s = SDL_CreateSurface(w, h, format);
  EXPECT_NE(s, nullptr);
  return s;
}

void Fill(SDL_Surface* s, const SDL_Rect rect, const Rgb c, const uint8_t a = 255) {
  SDL_FillSurfaceRect(s, &rect, SDL_MapSurfaceRGBA(s, c.r, c.g, c.b, a));
}

/** Four quadrants: red top left, green top right, blue bottom left, yellow bottom right. */
SDL_Surface* Quadrants(const int w, const int h) {
  SDL_Surface* s = Filled(w, h, SDL_PIXELFORMAT_RGB24);
  Fill(s, {0, 0, w / 2, h / 2}, kRed);
  Fill(s, {w / 2, 0, w - w / 2, h / 2}, kGreen);
  Fill(s, {0, h / 2, w / 2, h - h / 2}, kBlue);
  Fill(s, {w / 2, h / 2, w - w / 2, h - h / 2}, kYellow);
  return s;
}

std::vector<uint8_t> Drain(SDL_IOStream* io) {
  std::vector<uint8_t> bytes(static_cast<size_t>(SDL_GetIOSize(io)));
  SDL_SeekIO(io, 0, SDL_IO_SEEK_SET);
  SDL_ReadIO(io, bytes.data(), bytes.size());
  SDL_CloseIO(io);
  return bytes;
}

std::vector<uint8_t> PngBytes(SDL_Surface* s) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  EXPECT_TRUE(IMG_SavePNG_IO(s, io, false)) << SDL_GetError();
  return Drain(io);
}

std::vector<uint8_t> JpegBytes(SDL_Surface* s) {
  SDL_IOStream* io = SDL_IOFromDynamicMem();
  EXPECT_TRUE(IMG_SaveJPG_IO(s, io, false, 95)) << SDL_GetError();
  return Drain(io);
}

/** Puts an EXIF segment holding `orientation` right after the JPEG's start marker. */
void TagOrientation(std::vector<uint8_t>& jpeg, const int orientation) {
  const std::vector<uint8_t> segment = {
      0xFF, 0xE1, 0x00, 0x22, 'E', 'x',  'i',  'f', 0, 0, 'M', 'M', 0, 0x2A, 0,
      0,    0,    8,    0,    1,   0x01, 0x12, 0,   3, 0, 0,   0,   1, 0,    static_cast<uint8_t>(orientation),
      0,    0,    0,    0,    0,   0};
  jpeg.insert(jpeg.begin() + 2, segment.begin(), segment.end());
}

class ProfileIconImagePrepTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_profile_icon_prep_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string Write(const std::string& name, const std::vector<uint8_t>& bytes) {
    const std::string path = (dir_ / name).string();
    EXPECT_TRUE(SDL_SaveFile(path.c_str(), bytes.data(), bytes.size())) << SDL_GetError();
    return path;
  }

  /** The prepared bytes decoded again. */
  static SDL_Surface* Decode(const EncodedProfileIcon& icon) {
    SDL_Surface* s = IMG_Load_IO(SDL_IOFromConstMem(icon.bytes.data(), icon.bytes.size()), true);
    EXPECT_NE(s, nullptr) << SDL_GetError();
    return s;
  }

  static void ExpectNear(SDL_Surface* s, const int x, const int y, const Rgb want) {
    uint8_t r = 0, g = 0, b = 0, a = 0;
    ASSERT_TRUE(SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) << x << "," << y;
    EXPECT_NEAR(r, want.r, 40) << "at " << x << "," << y;
    EXPECT_NEAR(g, want.g, 40) << "at " << x << "," << y;
    EXPECT_NEAR(b, want.b, 40) << "at " << x << "," << y;
  }

  std::filesystem::path dir_;
};

TEST_F(ProfileIconImagePrepTest, APortraitPhotoTaggedToTurnComesOutUpright) {
  // Stored 200 x 100, left half red and right half blue, tagged "turn a quarter": upright it is 100 x 200 with red
  // on top and blue below. Its centre square then has red in its top half and blue in its bottom half; read without
  // the tag it would be red left / blue right.
  SDL_Surface* s = Filled(200, 100, SDL_PIXELFORMAT_RGB24);
  Fill(s, {0, 0, 100, 100}, kRed);
  Fill(s, {100, 0, 100, 100}, kBlue);
  std::vector<uint8_t> jpeg = JpegBytes(s);
  SDL_DestroySurface(s);
  TagOrientation(jpeg, 6);

  auto icon = PrepareProfileIconFromFile(Write("portrait.jpg", jpeg));
  ASSERT_TRUE(icon) << icon.error().message;
  EXPECT_EQ(icon->content_type, "image/jpeg");
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 100);
  EXPECT_EQ(out->h, 100);
  ExpectNear(out, 50, 10, kRed);
  ExpectNear(out, 50, 90, kBlue);
  SDL_DestroySurface(out);
}

TEST_F(ProfileIconImagePrepTest, AThreeByFourPictureComesOutSquareAndItIsTheMiddle) {
  // Portrait 300 x 400: red top quarter, green middle half, blue bottom quarter. The middle square is rows 50..350.
  SDL_Surface* tall = Filled(300, 400, SDL_PIXELFORMAT_RGB24);
  Fill(tall, {0, 0, 300, 100}, kRed);
  Fill(tall, {0, 100, 300, 200}, kGreen);
  Fill(tall, {0, 300, 300, 100}, kBlue);
  const std::vector<uint8_t> tall_png = PngBytes(tall);
  SDL_DestroySurface(tall);
  auto icon = PrepareProfileIconFromFile(Write("tall.png", tall_png));
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->w, out->h);
  EXPECT_EQ(out->w, 300) << "the square is 300 px, under the 512 limit, so it is not enlarged";
  ExpectNear(out, 150, 150, kGreen);
  ExpectNear(out, 150, 3, kRed); // 50 of the top 100 rows are inside the square
  ExpectNear(out, 150, 296, kBlue);
  SDL_DestroySurface(out);

  // Landscape 400 x 300, the same bands turned: columns 50..350 are kept.
  SDL_Surface* wide = Filled(400, 300, SDL_PIXELFORMAT_RGB24);
  Fill(wide, {0, 0, 100, 300}, kRed);
  Fill(wide, {100, 0, 200, 300}, kGreen);
  Fill(wide, {300, 0, 100, 300}, kBlue);
  const std::vector<uint8_t> wide_png = PngBytes(wide);
  SDL_DestroySurface(wide);
  icon = PrepareProfileIconFromFile(Write("wide.png", wide_png));
  ASSERT_TRUE(icon) << icon.error().message;
  out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 300);
  EXPECT_EQ(out->h, 300);
  ExpectNear(out, 150, 150, kGreen);
  ExpectNear(out, 3, 150, kRed);
  ExpectNear(out, 296, 150, kBlue);
  SDL_DestroySurface(out);
}

TEST_F(ProfileIconImagePrepTest, ALargePictureIsScaledToTheDefaultLimitOf512) {
  // The photo is viewed large on the profile page, so the stored side is 512 (it was 256); a given limit still wins.
  SDL_Surface* s = Quadrants(1200, 900);
  const std::string path = Write("big.png", PngBytes(s));
  SDL_DestroySurface(s);
  auto icon = PrepareProfileIconFromFile(path);
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 512);
  EXPECT_EQ(out->h, 512);
  SDL_DestroySurface(out);
  icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256);
  ASSERT_TRUE(icon);
  out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 256);
  SDL_DestroySurface(out);
}

TEST_F(ProfileIconImagePrepTest, AGivenRectangleYieldsThatRegion) {
  SDL_Surface* s = Quadrants(400, 300);
  const std::string path = Write("quads.png", PngBytes(s));
  SDL_DestroySurface(s);
  struct Case {
    SDL_Rect rect;
    Rgb colour;
  };
  const Case cases[] = {{{0, 0, 200, 150}, kRed},
                        {{200, 0, 200, 150}, kGreen},
                        {{0, 150, 200, 150}, kBlue},
                        {{200, 150, 200, 150}, kYellow}};
  for (const Case& c : cases) {
    auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, c.rect);
    ASSERT_TRUE(icon) << icon.error().message;
    SDL_Surface* out = Decode(*icon);
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->w, 200) << "the rectangle's shape is kept, and a small part is not scaled up";
    EXPECT_EQ(out->h, 150);
    ExpectNear(out, 5, 5, c.colour);
    ExpectNear(out, 194, 5, c.colour);
    ExpectNear(out, 5, 144, c.colour);
    ExpectNear(out, 194, 144, c.colour);
    SDL_DestroySurface(out);
  }
  // A square straddling the centre shows all four.
  auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, SDL_Rect{100, 50, 200, 200});
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  ASSERT_EQ(out->w, 200);
  ExpectNear(out, 20, 20, kRed);
  ExpectNear(out, 180, 20, kGreen);
  ExpectNear(out, 20, 180, kBlue);
  ExpectNear(out, 180, 180, kYellow);
  SDL_DestroySurface(out);
}

TEST_F(ProfileIconImagePrepTest, ARectangleOutsideThePictureIsClampedAndAnEmptyOneIsAnError) {
  SDL_Surface* s = Quadrants(400, 300);
  const std::string path = Write("quads.png", PngBytes(s));
  SDL_DestroySurface(s);

  // Reaching out over the top left: only the red quadrant is inside.
  auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, SDL_Rect{-100, -100, 300, 250});
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  ExpectNear(out, 5, 5, kRed);
  ExpectNear(out, out->w - 5, out->h - 5, kRed);
  SDL_DestroySurface(out);

  // Reaching out over the bottom right: the yellow corner.
  icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, SDL_Rect{300, 200, 500, 500});
  ASSERT_TRUE(icon) << icon.error().message;
  out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, out->h) << "100 x 100 inside";
  ExpectNear(out, 5, 5, kYellow);
  SDL_DestroySurface(out);

  for (const SDL_Rect bad :
       {SDL_Rect{10, 10, 0, 0}, SDL_Rect{10, 10, -5, 20}, SDL_Rect{1000, 1000, 10, 10}, SDL_Rect{-50, -50, 20, 20}}) {
    icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, bad);
    ASSERT_FALSE(icon) << bad.x << "," << bad.y << " " << bad.w << "x" << bad.h;
    EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_unreadable"));
  }
}

TEST_F(ProfileIconImagePrepTest, ATransparentPngStaysAPngWithItsTransparency) {
  SDL_Surface* s = Filled(300, 200, SDL_PIXELFORMAT_RGBA32);
  Fill(s, {0, 0, 300, 200}, kRed, 255);
  Fill(s, {0, 0, 300, 50}, kRed, 0); // the top quarter fully clear
  const std::string path = Write("clear.png", PngBytes(s));
  SDL_DestroySurface(s);

  auto icon = PrepareProfileIconFromFile(path);
  ASSERT_TRUE(icon) << icon.error().message;
  EXPECT_EQ(icon->content_type, "image/png");
  EXPECT_EQ(icon->kind, "image/png");
  EXPECT_EQ(icon->file_extension, "png");
  SDL_Surface* out = Decode(*icon);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->w, 200);
  EXPECT_EQ(out->h, 200);
  uint8_t r = 0, g = 0, b = 0, a = 255;
  ASSERT_TRUE(SDL_ReadSurfacePixel(out, 128, 5, &r, &g, &b, &a));
  EXPECT_EQ(a, 0) << "still clear at the top";
  ASSERT_TRUE(SDL_ReadSurfacePixel(out, 128, 190, &r, &g, &b, &a));
  EXPECT_EQ(a, 255);
  EXPECT_GT(r, 200);
  SDL_DestroySurface(out);
}

TEST_F(ProfileIconImagePrepTest, ATooLargeResultSaysSoInThePersonsWords) {
  SDL_Surface* opaque = Quadrants(300, 300);
  const std::string jpeg_source = Write("big.png", PngBytes(opaque));
  SDL_DestroySurface(opaque);
  auto icon = PrepareProfileIconFromFile(jpeg_source, /*max_bytes=*/50);
  ASSERT_FALSE(icon);
  EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_too_large"));
  EXPECT_FALSE(icon.error().user.empty());

  SDL_Surface* clear = Filled(300, 300, SDL_PIXELFORMAT_RGBA32);
  Fill(clear, {0, 0, 300, 300}, kBlue, 128);
  const std::string png_source = Write("bigclear.png", PngBytes(clear));
  SDL_DestroySurface(clear);
  icon = PrepareProfileIconFromFile(png_source, /*max_bytes=*/50);
  ASSERT_FALSE(icon);
  EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_too_large"));
}

TEST_F(ProfileIconImagePrepTest, AFileThatIsNoPictureOrIsMissingSaysItCannotBeOpened) {
  auto icon = PrepareProfileIconFromFile(Write("junk.jpg", std::vector<uint8_t>(64, 0x5A)));
  ASSERT_FALSE(icon);
  EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_unreadable"));
  EXPECT_FALSE(icon.error().user.empty());
  icon = PrepareProfileIconFromFile((dir_ / "missing.jpg").string());
  ASSERT_FALSE(icon);
  EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_unreadable"));
}

// The crop page's choice (PLAN-avatar-edit items 10 and 11): decode upright -> SampleAffine -> scale -> encode.
TEST_F(ProfileIconImagePrepTest, TheIdentityAffineIsTheCentredSquare) {
  SDL_Surface* s = Quadrants(200, 100);
  const std::string path = Write("quad.png", PngBytes(s));
  SDL_DestroySurface(s);
  ProfileIconCrop crop;
  crop.side = 100;
  crop.affine.x0 = 50.0; // the middle square: x 50..150, y 0..100
  auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, std::nullopt, crop);
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* got = Decode(icon.value());
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->w, 100);
  EXPECT_EQ(got->h, 100);
  ExpectNear(got, 10, 10, kRed);
  ExpectNear(got, 90, 10, kGreen);
  ExpectNear(got, 10, 90, kBlue);
  ExpectNear(got, 90, 90, kYellow);
  SDL_DestroySurface(got);
}

TEST_F(ProfileIconImagePrepTest, ATurnedAffineMovesTheQuadrantsAQuarterTurnClockwise) {
  SDL_Surface* s = Quadrants(200, 100);
  const std::string path = Write("quad.png", PngBytes(s));
  SDL_DestroySurface(s);
  // The picture turned 90 degrees clockwise about its middle: output (u, v) reads the source at (v + 50, 100 - u).
  ProfileIconCrop crop;
  crop.side = 100;
  crop.affine = {0.0, 1.0, 50.0, -1.0, 0.0, 100.0};
  auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, std::nullopt, crop);
  ASSERT_TRUE(icon) << icon.error().message;
  SDL_Surface* got = Decode(icon.value());
  ASSERT_NE(got, nullptr);
  ExpectNear(got, 90, 10, kRed);    // red was top left, now top right
  ExpectNear(got, 90, 90, kGreen);  // green was top right, now bottom right
  ExpectNear(got, 10, 10, kBlue);   // blue was bottom left, now top left
  ExpectNear(got, 10, 90, kYellow); // yellow was bottom right, now bottom left
  SDL_DestroySurface(got);
}

TEST_F(ProfileIconImagePrepTest, AnAffineWithNoSidesAndAMissingFileAreErrors) {
  SDL_Surface* s = Quadrants(64, 64);
  const std::string path = Write("quad.png", PngBytes(s));
  SDL_DestroySurface(s);
  ProfileIconCrop empty;
  empty.side = 0;
  auto icon = PrepareProfileIconFromFile(path, 512u * 1024u, 256, std::nullopt, empty);
  ASSERT_FALSE(icon);
  EXPECT_EQ(icon.error().user, Tr("settings.profile.photo_unreadable"));
  ProfileIconCrop one;
  one.side = 10;
  icon = PrepareProfileIconFromFile((dir_ / "missing.jpg").string(), 512u * 1024u, 256, std::nullopt, one);
  ASSERT_FALSE(icon);
}

TEST_F(ProfileIconImagePrepTest, ThePreviewIsUprightLimitedAndKeepsTheRealSize) {
  SDL_Surface* s = Quadrants(300, 100);
  const std::string path = Write("quad.png", PngBytes(s));
  SDL_DestroySurface(s);
  auto preview = DecodeProfilePhotoPreview(path, 150);
  ASSERT_TRUE(preview) << preview.error().message;
  EXPECT_EQ(preview->full_width, 300);
  EXPECT_EQ(preview->full_height, 100);
  EXPECT_EQ(preview->width, 150);
  EXPECT_EQ(preview->height, 50);
  ASSERT_EQ(preview->rgba.size(), 150u * 50u * 4u);
  // Top left pixel is red (RGBA order).
  EXPECT_GT(preview->rgba[0], 200);
  EXPECT_LT(preview->rgba[1], 60);
  EXPECT_FALSE(DecodeProfilePhotoPreview(Write("junk.jpg", std::vector<uint8_t>(64, 0x5A))));
}

} // namespace
} // namespace pbr
