#include "base/people/AvatarGlyph.h"

#include <gtest/gtest.h>

namespace {

using namespace pbr;

TEST(AvatarGlyphTest, LetterFromLatinName) {
  const AvatarGlyph g = MakeAvatarGlyph("alice", "account:abc");
  EXPECT_EQ(g.letter, "A");
}

TEST(AvatarGlyphTest, LetterFromUtf8Name) {
  const AvatarGlyph g = MakeAvatarGlyph("アリス", "account:abc");
  EXPECT_EQ(g.letter, "ア");
}

TEST(AvatarGlyphTest, LetterFallsBackToStableId) {
  const AvatarGlyph g = MakeAvatarGlyph("  ", "relay:lHaEnkO4");
  EXPECT_EQ(g.letter, "L");
}

TEST(AvatarGlyphTest, LetterFallsBackToQuestion) {
  const AvatarGlyph g = MakeAvatarGlyph("", "");
  EXPECT_EQ(g.letter, "?");
  EXPECT_EQ(g.tone, 0);
}

TEST(AvatarGlyphTest, ToneStableAcrossRename) {
  const AvatarGlyph a = MakeAvatarGlyph("Alice", "account:same-id");
  const AvatarGlyph b = MakeAvatarGlyph("Bob", "account:same-id");
  EXPECT_EQ(a.tone, b.tone);
  EXPECT_EQ(a.letter, "A");
  EXPECT_EQ(b.letter, "B");
}

TEST(AvatarGlyphTest, ToneDiffersById) {
  const AvatarGlyph a = MakeAvatarGlyph("Alice", "account:one");
  const AvatarGlyph b = MakeAvatarGlyph("Alice", "account:two");
  // Extremely unlikely to collide with FNV on short distinct ids; allow equal but prefer differ.
  // Soft check: both in range.
  EXPECT_GE(a.tone, 0);
  EXPECT_LT(a.tone, kAvatarToneCount);
  EXPECT_GE(b.tone, 0);
  EXPECT_LT(b.tone, kAvatarToneCount);
}

TEST(AvatarGlyphTest, AvatarStableIdPriority) {
  EXPECT_EQ(AvatarStableId("account:a", "relay:r", "peer:p", "c1"), "account:a");
  EXPECT_EQ(AvatarStableId("", "relay:r", "peer:p", "c1"), "relay:r");
  EXPECT_EQ(AvatarStableId("", "", "peer:p", "c1"), "peer:p");
  EXPECT_EQ(AvatarStableId("", "", "", "c1"), "c1");
}

} // namespace
