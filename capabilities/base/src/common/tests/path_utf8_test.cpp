#include "common/PathUtf8.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(PathUtf8Test, ACjkAndEmojiNameRoundTripsAndKeepsItsParts) {
  const std::string name = "\xE6\xAD\x8C \xE6\x88\x91\xE7\x9A\x84\xF0\x9F\x8E\xB5.MP3"; // "歌 我的🎵.MP3"
  const std::filesystem::path path = PathFromUtf8("dir") / PathFromUtf8(name);
  EXPECT_EQ(PathToUtf8(path.filename()), name);
  EXPECT_EQ(PathToUtf8(path.extension()), ".MP3");
  EXPECT_EQ(PathToUtf8(PathFromUtf8(name)), name);
  EXPECT_EQ(PathToUtf8(PathFromUtf8("")), "");
}

} // namespace
} // namespace pbr
