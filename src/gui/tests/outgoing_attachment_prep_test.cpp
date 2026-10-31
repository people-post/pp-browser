#include "foundation/platform/ImageMetadata.h"
#include "gui/chat/OutgoingAttachmentPrep.h"
#include "lib/media/VoiceFile.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <gtest/gtest.h>

#include <algorithm>
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

void WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool Contains(const std::vector<uint8_t>& bytes, const std::string& needle) {
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

SDL_Surface* Noisy(const int w, const int h) {
  SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGB24);
  uint32_t state = 99;
  for (int y = 0; y < h; ++y) {
    auto* row = static_cast<uint8_t*>(s->pixels) + static_cast<size_t>(y) * s->pitch;
    for (int x = 0; x < w * 3; ++x) {
      state = state * 1664525u + 1013904223u;
      row[x] = static_cast<uint8_t>(state >> 24);
    }
  }
  return s;
}

/** Exif segment: orientation + a fake GPS block that must not survive an original send. */
std::vector<uint8_t> ExifWithGps(const int orientation) {
  std::vector<uint8_t> tiff = {'M', 'M', 0, 0x2A, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1,
                               0,   static_cast<uint8_t>(orientation), 0, 0, 0, 0, 0, 0};
  const std::string gps = "GPSLATITUDE-31.2304N";
  tiff.insert(tiff.end(), gps.begin(), gps.end());
  std::vector<uint8_t> payload = {'E', 'x', 'i', 'f', 0, 0};
  payload.insert(payload.end(), tiff.begin(), tiff.end());
  const uint16_t len = static_cast<uint16_t>(payload.size() + 2);
  std::vector<uint8_t> segment = {0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len & 0xFF)};
  segment.insert(segment.end(), payload.begin(), payload.end());
  return segment;
}

class OutgoingAttachmentPrepTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp_outgoing_prep-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::string SaveJpeg(SDL_Surface* s, const std::string& name, const int orientation = 0) {
    const std::string path = (dir_ / name).string();
    EXPECT_TRUE(IMG_SaveJPG(s, path.c_str(), 90)) << SDL_GetError();
    SDL_DestroySurface(s);
    if (orientation > 0) {
      auto jpeg = ReadFile(path);
      const auto exif = ExifWithGps(orientation);
      jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());
      WriteFile(path, jpeg);
    }
    return path;
  }

  std::filesystem::path dir_;
  AttachmentLimits limits_;
};

TEST_F(OutgoingAttachmentPrepTest, ABigPhotoIsShrunkIntoTheImageLimit) {
  const std::string path = SaveJpeg(Noisy(3000, 2000), "IMG_0001.jpg");
  ASSERT_GT(std::filesystem::file_size(path), limits_.max_image_bytes) << "the sample must need shrinking";
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_TRUE(out->image_is_shrunk);
  EXPECT_EQ(out->mime, "image/jpeg");
  EXPECT_EQ(out->filename, "IMG_0001.jpg");
  EXPECT_LE(out->bytes.size(), limits_.max_image_bytes);
  const auto info = ReadImageHeaderBytes(out->bytes);
  ASSERT_TRUE(info);
  EXPECT_LE(std::max(info->width, info->height), kShrunkImageLongEdge);
}

TEST_F(OutgoingAttachmentPrepTest, ShrinkingTurnsAPhotoUprightAndDropsItsMetadata) {
  const std::string path = SaveJpeg(Noisy(200, 100), "sideways.jpg", /*orientation=*/6);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  const auto info = ReadImageHeaderBytes(out->bytes);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->width, 100) << "a quarter turn: portrait now";
  EXPECT_EQ(info->height, 200);
  EXPECT_EQ(info->exif_orientation, 1);
  EXPECT_FALSE(Contains(out->bytes, "GPSLATITUDE"));
  EXPECT_FALSE(Contains(out->bytes, "Exif"));
}

TEST_F(OutgoingAttachmentPrepTest, AnAlphaPngStaysAPngAndTakesThePngName) {
  SDL_Surface* s = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
  SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGBA(s, 10, 200, 30, 128));
  const std::string path = (dir_ / "sticker.PNG").string();
  ASSERT_TRUE(IMG_SavePNG(s, path.c_str()));
  SDL_DestroySurface(s);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "image/png");
  EXPECT_EQ(out->filename, "sticker.png");
}

TEST_F(OutgoingAttachmentPrepTest, OriginalJpegKeepsItsPixelsButLosesItsLocation) {
  const std::string path = SaveJpeg(Noisy(120, 90), "orig.jpg", /*orientation=*/6);
  const auto file = ReadFile(path);
  ASSERT_TRUE(Contains(file, "GPSLATITUDE"));
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_FALSE(out->image_is_shrunk);
  EXPECT_EQ(out->mime, "image/jpeg");
  EXPECT_EQ(out->filename, "orig.jpg");
  EXPECT_FALSE(Contains(out->bytes, "GPSLATITUDE"));
  const auto info = ReadImageHeaderBytes(out->bytes);
  ASSERT_TRUE(info);
  EXPECT_EQ(info->width, 120) << "not re-encoded: same pixels";
  EXPECT_EQ(info->exif_orientation, 6) << "still shows the right way up";
}

TEST_F(OutgoingAttachmentPrepTest, OriginalPngGoesAsItIs) {
  SDL_Surface* s = Noisy(40, 40);
  const std::string path = (dir_ / "shot.png").string();
  ASSERT_TRUE(IMG_SavePNG(s, path.c_str()));
  SDL_DestroySurface(s);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->bytes, ReadFile(path));
  EXPECT_EQ(out->mime, "image/png");
}

// "Photo" now offers videos on a phone: a .mov (HEVC) goes as it is, typed as video, never transcoded or refused.
TEST_F(OutgoingAttachmentPrepTest, AVideoFromThePhotoLibraryGoesAsItIsAsAFile) {
  limits_.max_video_bytes = 100'000'000;
  std::vector<uint8_t> mov = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'q', 't', ' ', ' ', 0, 0, 0, 0};
  mov.resize(5000, 0x5a);
  const std::string path = (dir_ / "IMG_0042.MOV").string();
  WriteFile(path, mov);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Photo);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "video/quicktime");
  EXPECT_EQ(out->filename, "IMG_0042.MOV");
  EXPECT_EQ(out->bytes, mov);
  EXPECT_FALSE(out->image_is_shrunk);
}

// A pick that carries no name (an Android media-store URI, copied to a file called "attachment") is named by what
// is in it, so a video is a video and a photo a photo.
TEST_F(OutgoingAttachmentPrepTest, ANamelessPickIsTypedByItsContent) {
  limits_.max_video_bytes = 100'000'000;
  std::vector<uint8_t> mp4 = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'i', 's', 'o', 'm', 0, 0, 0, 0};
  mp4.resize(4000, 0x11);
  const std::string path = (dir_ / "attachment").string();
  WriteFile(path, mp4);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Photo);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "video/mp4");
  EXPECT_EQ(out->filename, "attachment.mp4");
}

// The content:// route end to end on a plain file: the pick is copied to the scratch folder, prepared from the
// copy, and the copy is gone afterwards (the person's file is untouched).
TEST_F(OutgoingAttachmentPrepTest, ACopiedPickIsPreparedAndTheCopyRemoved) {
  const std::string path = (dir_ / "note.pdf").string();
  WriteFile(path, std::vector<uint8_t>(300, 7));
  const std::string scratch = (dir_ / "picked").string();
  // A plain path is used where it is; the scratch folder stays unused.
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File, scratch);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->filename, "note.pdf");
  EXPECT_FALSE(std::filesystem::exists(scratch));
  // An unreadable content:// pick is an error the person can read, not a crash.
  auto gone = PrepareOutgoingAttachment("content://nowhere/document/video%3A1", limits_, OutgoingPickSource::Photo, scratch);
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().code, static_cast<int32_t>(OutgoingAttachmentError::Unreadable));
  EXPECT_FALSE(gone.error().user.empty());
}

TEST_F(OutgoingAttachmentPrepTest, AGifIsNeverShrunk) {
  std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 4, 0, 4, 0, 0, 0, 0, ';'};
  const std::string path = (dir_ / "loop.gif").string();
  WriteFile(path, gif);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "image/gif");
  EXPECT_EQ(out->bytes, gif);
  EXPECT_FALSE(out->image_is_shrunk);
}

TEST_F(OutgoingAttachmentPrepTest, FromFileAPictureNamedButNotBuiltLikeOneGoesAsTheFileItIs) {
  const std::string path = (dir_ / "broken.jpg").string();
  WriteFile(path, {'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c'});
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "application/octet-stream") << "never claimed to be an image";
  EXPECT_FALSE(out->image_is_shrunk);
}

TEST_F(OutgoingAttachmentPrepTest, FromFileAJpegThatCannotBeDecodedGoesAsTheFileTheChoiceSaid) {
  // JPEG magic, then nothing a decoder can use.
  const std::string path = (dir_ / "cut.jpg").string();
  WriteFile(path, {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 'a', 'b', 0xFF, 0xD9});
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "application/octet-stream");
  EXPECT_EQ(out->filename, "cut.jpg");
}

TEST_F(OutgoingAttachmentPrepTest, FromPhotoAPictureThatCannotBeDecodedIsNotSentAtAll) {
  const std::string undecodable = (dir_ / "cut.jpg").string();
  WriteFile(undecodable, {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 'a', 'b', 0xFF, 0xD9});
  auto out = PrepareOutgoingAttachment(undecodable, limits_, OutgoingPickSource::Photo);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().code, static_cast<int32_t>(OutgoingAttachmentError::ImageUnprocessable));
  EXPECT_FALSE(out.error().user.empty()) << "the person is told to use 'File'";

  const std::string text_named_jpg = (dir_ / "fake.jpg").string();
  WriteFile(text_named_jpg, {'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c'});
  auto fake = PrepareOutgoingAttachment(text_named_jpg, limits_, OutgoingPickSource::Photo);
  ASSERT_FALSE(fake);
  EXPECT_EQ(fake.error().code, static_cast<int32_t>(OutgoingAttachmentError::ImageUnprocessable));
}

TEST_F(OutgoingAttachmentPrepTest, AJpegWithoutAnExtensionIsStillAPictureByItsContent) {
  const std::string source = SaveJpeg(Noisy(3000, 2000), "tmp.jpg", /*orientation=*/6);
  const std::string path = (dir_ / "IMG_0001").string();
  std::filesystem::copy_file(source, path);

  auto shrunk = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Photo);
  ASSERT_TRUE(shrunk) << shrunk.error().message;
  EXPECT_TRUE(shrunk->image_is_shrunk);
  EXPECT_EQ(shrunk->mime, "image/jpeg");
  EXPECT_EQ(shrunk->filename, "IMG_0001.jpg");
  EXPECT_FALSE(Contains(shrunk->bytes, "GPSLATITUDE"));

  auto original = PrepareOutgoingAttachment(path, [] { AttachmentLimits l; l.max_original_image_bytes = 100'000'000; return l; }(),
                                            OutgoingPickSource::Original);
  ASSERT_TRUE(original) << original.error().message;
  EXPECT_EQ(original->mime, "image/jpeg");
  EXPECT_FALSE(Contains(original->bytes, "GPSLATITUDE")) << "the location goes whatever the file is called";
}

TEST_F(OutgoingAttachmentPrepTest, AJpegCalledPngOrDatLosesItsLocationOnTheOriginalPath) {
  const std::string source = SaveJpeg(Noisy(120, 90), "tmp2.jpg", /*orientation=*/1);
  for (const char* name : {"photo.png", "photo.dat", "photo"}) {
    const std::string path = (dir_ / name).string();
    std::filesystem::copy_file(source, path, std::filesystem::copy_options::overwrite_existing);
    auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
    ASSERT_TRUE(out) << name << " " << out.error().message;
    EXPECT_EQ(out->mime, "image/jpeg") << name;
    EXPECT_FALSE(Contains(out->bytes, "GPSLATITUDE")) << name;
  }
}

TEST_F(OutgoingAttachmentPrepTest, OriginalFailsClosedWhenTheJpegCannotBeCleaned) {
  const std::string good = SaveJpeg(Noisy(120, 90), "g.jpg", /*orientation=*/6);
  auto bytes = ReadFile(good);
  bytes.resize(bytes.size() - 2); // the end-of-image marker is gone: malformed
  const std::string path = (dir_ / "damaged.jpg").string();
  WriteFile(path, bytes);
  ASSERT_TRUE(Contains(bytes, "GPSLATITUDE"));
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
  ASSERT_FALSE(out) << "the original bytes, location and all, must not go out";
  EXPECT_EQ(out.error().code, static_cast<int32_t>(OutgoingAttachmentError::ImageUnprocessable));
}

TEST_F(OutgoingAttachmentPrepTest, OriginalDropsSecondPictureAndTrailerOfTheFile) {
  auto bytes = ReadFile(SaveJpeg(Noisy(80, 60), "m.jpg", /*orientation=*/1));
  const std::string trailer = "TRAILER-GPS-DATA";
  const auto second = bytes;
  bytes.insert(bytes.end(), second.begin(), second.end());
  bytes.insert(bytes.end(), trailer.begin(), trailer.end());
  const std::string path = (dir_ / "mpf.jpg").string();
  WriteFile(path, bytes);
  limits_.max_original_image_bytes = 100'000'000;
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_FALSE(Contains(out->bytes, "TRAILER-GPS-DATA"));
  EXPECT_LT(out->bytes.size(), bytes.size() / 2 + 8) << "one picture only";
}

TEST_F(OutgoingAttachmentPrepTest, HeicIsAFileNotAPicture) {
  const std::string path = (dir_ / "IMG_9.HEIC").string();
  WriteFile(path, std::vector<uint8_t>(500, 3));
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->mime, "application/octet-stream");
  EXPECT_EQ(out->filename, "IMG_9.HEIC");
  EXPECT_EQ(out->bytes.size(), 500u);
}

TEST_F(OutgoingAttachmentPrepTest, ATooBigFileIsRefusedBeforeItIsRead) {
  limits_.max_file_bytes = 100;
  const std::string path = (dir_ / "big.pdf").string();
  WriteFile(path, std::vector<uint8_t>(101, 1));
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().code, static_cast<int32_t>(OutgoingAttachmentError::TooLarge));
  EXPECT_FALSE(out.error().user.empty()) << "the person is told the limit";
  // Right at the limit goes through.
  WriteFile(path, std::vector<uint8_t>(100, 1));
  EXPECT_TRUE(PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File));
}

TEST_F(OutgoingAttachmentPrepTest, EachKindUsesItsOwnLimit) {
  limits_.max_file_bytes = 1000;
  limits_.max_video_bytes = 50;
  limits_.max_audio_bytes = 20;
  WriteFile((dir_ / "a.mp4").string(), std::vector<uint8_t>(51, 1));
  WriteFile((dir_ / "a.mp3").string(), std::vector<uint8_t>(21, 1));
  WriteFile((dir_ / "a.zip").string(), std::vector<uint8_t>(900, 1));
  EXPECT_FALSE(PrepareOutgoingAttachment((dir_ / "a.mp4").string(), limits_, OutgoingPickSource::File));
  EXPECT_FALSE(PrepareOutgoingAttachment((dir_ / "a.mp3").string(), limits_, OutgoingPickSource::File));
  EXPECT_TRUE(PrepareOutgoingAttachment((dir_ / "a.zip").string(), limits_, OutgoingPickSource::File));
}

TEST_F(OutgoingAttachmentPrepTest, AnOriginalPictureUsesTheOriginalLimit) {
  limits_.max_original_image_bytes = 100;
  const std::string path = SaveJpeg(Noisy(80, 80), "o.jpg");
  ASSERT_GT(std::filesystem::file_size(path), 100u);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::Original);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().code, static_cast<int32_t>(OutgoingAttachmentError::TooLarge));
  EXPECT_TRUE(PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File)) << "shrinking is not bound by it";
}

TEST_F(OutgoingAttachmentPrepTest, AHugePictureIsRefusedBeforeDecoding) {
  // A small file whose header says 100000 x 100000 pixels.
  std::vector<uint8_t> bomb = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                               0x00, 0x01, 0x86, 0xA0, 0x00, 0x01, 0x86, 0xA0, 8, 2, 0, 0, 0};
  const std::string path = (dir_ / "bomb.png").string();
  WriteFile(path, bomb);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_FALSE(out);
  EXPECT_EQ(out.error().code, static_cast<int32_t>(OutgoingAttachmentError::ImageFailed));
}

// A voice recording goes as a plain attachment: the name and the type are the whole wire convention.
TEST_F(OutgoingAttachmentPrepTest, ARecordedVoiceMessageIsAudioOggNamedByItsLength) {
  const std::vector<uint8_t> ogg = {'O', 'g', 'g', 'S', 0, 2, 1, 2, 3, 4, 5, 6, 7, 8};
  const std::string path = (dir_ / VoiceFileName(4200)).string();
  WriteFile(path, ogg);
  auto out = PrepareOutgoingAttachment(path, limits_, OutgoingPickSource::File);
  ASSERT_TRUE(out) << out.error().message;
  EXPECT_EQ(out->filename, "voice-4200.ogg");
  EXPECT_EQ(out->mime, kVoiceMime);
  EXPECT_EQ(out->mime, "audio/ogg");
  EXPECT_EQ(out->bytes, ogg);          // not re-encoded, not shrunk
  EXPECT_FALSE(out->image_is_shrunk);
  // The receiver's bubble decision accepts exactly what was sent.
  EXPECT_TRUE(DecideVoiceBubble(out->mime, out->filename, out->bytes.size(), limits_.voice_max_seconds).bubble);
}

} // namespace
} // namespace pbr
