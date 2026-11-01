#include "lib/media/VoiceFile.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(VoiceFile, NameAndMimeAreTheWireConvention) {
  EXPECT_EQ(VoiceFileName(4200), "voice-4200.ogg");
  EXPECT_EQ(VoiceFileName(0), "voice-0.ogg");
  EXPECT_EQ(VoiceFileName(-5), "voice-0.ogg");
  EXPECT_STREQ(kVoiceMime, "audio/ogg");
}

TEST(VoiceFile, ParsesOnlyTheExactName) {
  EXPECT_EQ(ParseVoiceFileName("voice-4200.ogg"), 4200);
  EXPECT_EQ(ParseVoiceFileName("voice-0.ogg"), 0);
  EXPECT_FALSE(ParseVoiceFileName("voice-.ogg"));
  EXPECT_FALSE(ParseVoiceFileName("voice-12a.ogg"));
  EXPECT_FALSE(ParseVoiceFileName("voice--5.ogg"));
  EXPECT_FALSE(ParseVoiceFileName("Voice-5.ogg"));
  EXPECT_FALSE(ParseVoiceFileName("voice-5.OGG"));
  EXPECT_FALSE(ParseVoiceFileName("voice-5.ogg.exe"));
  EXPECT_FALSE(ParseVoiceFileName("my-voice-5.ogg"));
  EXPECT_FALSE(ParseVoiceFileName("voice-1234567890.ogg")); // ten digits: not a real length
  EXPECT_FALSE(ParseVoiceFileName(""));
}

TEST(VoiceFile, BubbleForSmallMatchingFile) {
  const auto view = DecideVoiceBubble("audio/ogg", "voice-4200.ogg", 25000, 120);
  EXPECT_TRUE(view.bubble);
  EXPECT_EQ(view.duration_ms, 4200);
}

TEST(VoiceFile, FileRowForLargeDeclaredSize) {
  EXPECT_TRUE(DecideVoiceBubble("audio/ogg", "voice-4200.ogg", kVoiceBubbleMaxBytes, 120).bubble);
  EXPECT_FALSE(DecideVoiceBubble("audio/ogg", "voice-4200.ogg", kVoiceBubbleMaxBytes + 1, 120).bubble);
  EXPECT_FALSE(DecideVoiceBubble("audio/ogg", "voice-4200.ogg", 0, 120).bubble);
}

TEST(VoiceFile, FileRowForOtherNamesAndTypes) {
  EXPECT_FALSE(DecideVoiceBubble("audio/ogg", "song.ogg", 25000, 120).bubble);
  EXPECT_FALSE(DecideVoiceBubble("audio/ogg", "voice-abc.ogg", 25000, 120).bubble);
  EXPECT_FALSE(DecideVoiceBubble("audio/mpeg", "voice-4200.ogg", 25000, 120).bubble);
  EXPECT_FALSE(DecideVoiceBubble("application/octet-stream", "voice-4200.ogg", 25000, 120).bubble);
  EXPECT_TRUE(DecideVoiceBubble("Audio/OGG", "voice-4200.ogg", 25000, 120).bubble);
}

TEST(VoiceFile, ShownLengthIsClampedToTheVoiceLimit) {
  EXPECT_EQ(DecideVoiceBubble("audio/ogg", "voice-999999999.ogg", 25000, 120).duration_ms, 120000);
  EXPECT_EQ(DecideVoiceBubble("audio/ogg", "voice-999999999.ogg", 25000, 30).duration_ms, 30000);
  EXPECT_EQ(DecideVoiceBubble("audio/ogg", "voice-119999.ogg", 25000, 120).duration_ms, 119999);
}

TEST(VoiceFile, DurationReadsAsMinutesAndSeconds) {
  EXPECT_EQ(FormatVoiceDuration(0), "0:01");
  EXPECT_EQ(FormatVoiceDuration(1000), "0:01");
  EXPECT_EQ(FormatVoiceDuration(1001), "0:02");
  EXPECT_EQ(FormatVoiceDuration(7000), "0:07");
  EXPECT_EQ(FormatVoiceDuration(65000), "1:05");
  EXPECT_EQ(FormatVoiceDuration(120000), "2:00");
}

} // namespace
} // namespace pbr
