#include "foundation/platform/VideoPosterExtractor.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace pbr {
namespace {

TEST(VideoPosterExtractorTest, SoftVideoPosterJpegIsValidJpeg) {
  auto soft = SoftVideoPosterJpeg(480);
  ASSERT_TRUE(soft) << soft.error().message;
  ASSERT_GE(soft->size(), 2u);
  EXPECT_EQ((*soft)[0], 0xFF);
  EXPECT_EQ((*soft)[1], 0xD8);
}

TEST(VideoPosterExtractorTest, MissingFileFallsBackToSoftJpeg) {
  const std::string missing =
      (std::filesystem::temp_directory_path() / "pp_video_poster_missing_definitely_not_real.mp4").string();
  std::error_code ec;
  std::filesystem::remove(missing, ec);

  auto jpeg = ExtractVideoPosterJpeg(missing, 320);
  ASSERT_TRUE(jpeg) << jpeg.error().message;
  ASSERT_GE(jpeg->size(), 2u);
  EXPECT_EQ((*jpeg)[0], 0xFF);
  EXPECT_EQ((*jpeg)[1], 0xD8);
}

} // namespace
} // namespace pbr
