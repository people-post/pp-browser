#include "gui/chat/VoiceSendPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(VoiceSendPolicy, TheFileGoesOnlyAfterTheSendOrItsStagedCopy) {
  EXPECT_EQ(DecideVoiceFileFate(/*sent=*/true, 0, 0), VoiceFileFate::Delete);
  EXPECT_EQ(DecideVoiceFileFate(false, 0, 1), VoiceFileFate::Delete); // failed after staging: Retry has the copy
  EXPECT_EQ(DecideVoiceFileFate(false, 2, 3), VoiceFileFate::Delete);
}

TEST(VoiceSendPolicy, ARefusalBeforeStagingKeepsTheRecording) {
  EXPECT_EQ(DecideVoiceFileFate(false, 0, 0), VoiceFileFate::KeepForRetry); // the send-rate limit, a failed prepare
  EXPECT_EQ(DecideVoiceFileFate(false, 2, 2), VoiceFileFate::KeepForRetry); // an older staged send is not this one
}

// The composer comes back in the mode it had last time, on phones, and never over what the person is doing.
TEST(VoiceSendPolicy, TheRememberedVoiceModeComesBackOnlyWhereItIsSafe) {
  EXPECT_TRUE(RestoreVoiceMode(/*remembered_voice=*/true, /*phone=*/true, /*chat_can_voice=*/true, /*has_draft=*/false,
                               /*editing_message=*/false, /*voice_mode_now=*/false));
  EXPECT_FALSE(RestoreVoiceMode(false, true, true, false, false, false)) << "the keyboard was the last mode";
  EXPECT_FALSE(RestoreVoiceMode(true, false, true, false, false, false)) << "the desktop keeps click mode";
  EXPECT_FALSE(RestoreVoiceMode(true, true, false, false, false, false)) << "a chat that cannot take a voice message";
  EXPECT_FALSE(RestoreVoiceMode(true, true, true, true, false, false)) << "never over a draft";
  EXPECT_FALSE(RestoreVoiceMode(true, true, true, false, true, false)) << "never while a message is edited";
  EXPECT_FALSE(RestoreVoiceMode(true, true, true, false, false, true)) << "already in voice mode";
}

} // namespace
} // namespace pbr
