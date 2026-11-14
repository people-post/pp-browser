#include "foundation/platform/os/OsDeviceName.h"

#include "common/DeviceName.h"

#include <gtest/gtest.h>

#include <string>

namespace {

std::string Repeat(const std::string& unit, const size_t times) {
  std::string out;
  for (size_t i = 0; i < times; ++i) {
    out += unit;
  }
  return out;
}

} // namespace

TEST(DeviceNameTest, TrimsAndKeepsOrdinaryNames) {
  EXPECT_EQ(pbr::NormalizeDeviceName("  iPhone "), "iPhone");
  EXPECT_EQ(pbr::NormalizeDeviceName("Kenneth\xE2\x80\x99s MacBook"), "Kenneth\xE2\x80\x99s MacBook");
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE6\x88\x91\xE7\x9A\x84 iPhone"), "\xE6\x88\x91\xE7\x9A\x84 iPhone");
}

TEST(DeviceNameTest, DropsControlCharactersAndNewlines) {
  EXPECT_EQ(pbr::NormalizeDeviceName("a\nb\tc\r"), "abc");
  EXPECT_EQ(pbr::NormalizeDeviceName(std::string("x\0y", 3)), "xy");
  EXPECT_EQ(pbr::NormalizeDeviceName("a\xC2\x85"
                                     "b"),
            "ab"); // U+0085
  EXPECT_EQ(pbr::NormalizeDeviceName("a\xE2\x80\xA8"
                                     "b"),
            "ab"); // U+2028
}

TEST(DeviceNameTest, DropsBidiControlsAndZeroWidthCharactersButKeepsTheEmojiJoiner) {
  // Each of these alone between two letters vanishes: LRE/RLE/PDF/LRO/RLO, LRI/RLI/FSI/PDI, LRM, RLM, ALM, ZWSP,
  // ZWNJ, BOM.
  for (const char* mark : {"\xE2\x80\xAA", "\xE2\x80\xAB", "\xE2\x80\xAC", "\xE2\x80\xAD", "\xE2\x80\xAE",
                           "\xE2\x81\xA6", "\xE2\x81\xA7", "\xE2\x81\xA8", "\xE2\x81\xA9", "\xE2\x80\x8E",
                           "\xE2\x80\x8F", "\xD8\x9C", "\xE2\x80\x8B", "\xE2\x80\x8C", "\xEF\xBB\xBF"}) {
    EXPECT_EQ(pbr::NormalizeDeviceName(std::string("a") + mark + "b"), "ab") << mark;
  }
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE2\x80\xAEPhone\xE2\x80\xAC"), "Phone");
  // U+200D stays: the family emoji is intact.
  const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
  EXPECT_EQ(pbr::NormalizeDeviceName("a" + family), "a" + family);
}

TEST(DeviceNameTest, TheSpacesOfU2000ToU200ATrimLikeSpaces) {
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE2\x80\x80\xE2\x80\x83Mac\xE2\x80\x8A\xE2\x80\x89"),
            "Mac"); // en quad, em space, hair, thin
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE2\x80\x82\xE2\x80\x8A"), "");
}

TEST(DeviceNameTest, ANameOfOnlyInvisibleCharactersIsNoName) {
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE2\x80\xAE\xE2\x80\x8B\xEF\xBB\xBF\xE2\x81\xA6\xE2\x80\x8F"), "");
  EXPECT_EQ(pbr::NormalizeDeviceName(" \xE2\x80\x8B \xD8\x9C\xE2\x80\x8C"), "");
}

TEST(DeviceNameTest, NothingLeftMeansNoName) {
  EXPECT_EQ(pbr::NormalizeDeviceName(""), "");
  EXPECT_EQ(pbr::NormalizeDeviceName("   \n\t "), "");
  EXPECT_EQ(pbr::NormalizeDeviceName("\xE3\x80\x80\xC2\xA0"), ""); // ideographic and no-break space only
  EXPECT_EQ(pbr::NormalizeDeviceName("\xFF\xFE"), "");             // not UTF-8
  EXPECT_EQ(pbr::NormalizeDeviceName("ab\xE2\x80"), "");           // cut short
  EXPECT_EQ(pbr::NormalizeDeviceName("\xC0\xAF"), "");             // overlong
}

TEST(DeviceNameTest, CutsAtThirtyTwoCodePoints) {
  EXPECT_EQ(pbr::NormalizeDeviceName(Repeat("a", 40)), Repeat("a", 32));
  EXPECT_EQ(pbr::NormalizeDeviceName(Repeat("\xE4\xBD\xA0", 33)), Repeat("\xE4\xBD\xA0", 32));
  EXPECT_EQ(pbr::NormalizeDeviceName(Repeat("a", 31) + "\xF0\x9F\x98\x80"
                                                       "b"),
            Repeat("a", 31) + "\xF0\x9F\x98\x80");
  // A family emoji is 5 code points (man ZWJ woman ZWJ girl): cut after the 32nd, in the middle of it.
  const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
  EXPECT_EQ(pbr::NormalizeDeviceName(Repeat("a", 30) + family), Repeat("a", 30) + "\xF0\x9F\x91\xA8\xE2\x80\x8D");
}

TEST(DeviceNameTest, ASpaceRightAtTheCutIsTrimmed) {
  EXPECT_EQ(pbr::NormalizeDeviceName(Repeat("a", 31) + " bcd"), Repeat("a", 31));
}

TEST(OsDeviceNameTest, TheSystemNameIsAlreadyNormalizedAndMayBeEmpty) {
  const std::string name = pbr::os::SystemDeviceName();
  EXPECT_EQ(pbr::NormalizeDeviceName(name), name);
}
