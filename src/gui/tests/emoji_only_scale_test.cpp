// Emoji-only messages are drawn large without a bubble: 1-3 emoji about 3x, 4-6 about 2x, anything with text normal.
#include "gui/chat/EmojiOnlyScale.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(EmojiOnlyScaleTest, OneToThreeEmojiAreExtraLarge) {
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\x8D"), EmojiScale::ExtraLarge);                                  // thumbs up
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\x8D\xF0\x9F\x91\x8D\xF0\x9F\x91\x8D"), EmojiScale::ExtraLarge);
  EXPECT_EQ(EmojiOnlyScale("\xE2\x9D\xA4\xEF\xB8\x8F"), EmojiScale::ExtraLarge);                          // red heart + VS16
}

TEST(EmojiOnlyScaleTest, FourToSixEmojiAreLarge) {
  std::string four;
  std::string seven;
  for (int i = 0; i < 7; ++i) {
    seven += "\xF0\x9F\x98\x80";
    if (i < 4) {
      four += "\xF0\x9F\x98\x80";
    }
  }
  EXPECT_EQ(EmojiOnlyScale(four), EmojiScale::Large);
  EXPECT_EQ(EmojiOnlyScale(seven), EmojiScale::None);
}

TEST(EmojiOnlyScaleTest, ClustersCountAsOneGrapheme) {
  // Thumbs up + medium skin tone; family (man ZWJ woman ZWJ girl); US flag (two regional indicators).
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"), EmojiScale::ExtraLarge);
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"), EmojiScale::ExtraLarge);
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"), EmojiScale::ExtraLarge);
  // Four flags are four graphemes.
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xA8\xF0\x9F\x87\xB3\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5"
                           "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"),
            EmojiScale::Large);
}

TEST(EmojiOnlyScaleTest, SymbolsThatAreTextAreNotEmoji) {
  EXPECT_EQ(EmojiOnlyScale("\xE2\x86\x92"), EmojiScale::None);  // right arrow
  EXPECT_EQ(EmojiOnlyScale("\xE2\x9C\x93"), EmojiScale::None);  // check mark (text style)
  EXPECT_EQ(EmojiOnlyScale("\xC2\xA9"), EmojiScale::None);       // (c)
  EXPECT_EQ(EmojiOnlyScale("\xE2\x84\xA2"), EmojiScale::None);  // (tm)
  EXPECT_EQ(EmojiOnlyScale("\xE2\x8C\x98"), EmojiScale::None);  // place of interest sign (technical)
  EXPECT_EQ(EmojiOnlyScale("\xE2\x9C\x93\xEF\xB8\x8F"), EmojiScale::ExtraLarge) << "VS16 asks for the emoji";
}

TEST(EmojiOnlyScaleTest, KeycapAndAlwaysEmojiSymbols) {
  EXPECT_EQ(EmojiOnlyScale("1\xEF\xB8\x8F\xE2\x83\xA3"), EmojiScale::ExtraLarge);  // keycap 1
  EXPECT_EQ(EmojiOnlyScale("1\xEF\xB8\x8F\xE2\x83\xA3#\xEF\xB8\x8F\xE2\x83\xA3"), EmojiScale::ExtraLarge);
  EXPECT_EQ(EmojiOnlyScale("1\xEF\xB8\x8F\xE2\x83\xA3 1"), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("\xE2\x9D\xA4"), EmojiScale::ExtraLarge);  // bare heart
  EXPECT_EQ(EmojiOnlyScale("\xE2\xAD\x90"), EmojiScale::ExtraLarge);  // star
}

TEST(EmojiOnlyScaleTest, SpacesBetweenEmojiAreIgnored) {
  EXPECT_EQ(EmojiOnlyScale(" \xF0\x9F\x91\x8D \xF0\x9F\x91\x8D\n"), EmojiScale::ExtraLarge);
}

TEST(EmojiOnlyScaleTest, LettersDigitsAndPunctuationAreNotEmojiOnly) {
  EXPECT_EQ(EmojiOnlyScale(""), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("   "), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("ok \xF0\x9F\x91\x8D"), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\x8D!"), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("1"), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("\xF0\x9F\x91\x8D""2"), EmojiScale::None);
  EXPECT_EQ(EmojiOnlyScale("\xE5\xA5\xBD"), EmojiScale::None);  // a CJK character
  EXPECT_EQ(EmojiOnlyScale("\xFF\xFE"), EmojiScale::None);      // invalid UTF-8
}

} // namespace
} // namespace pbr
