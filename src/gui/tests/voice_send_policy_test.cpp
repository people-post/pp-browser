#include "gui/chat/VoiceSendPolicy.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(VoiceSendPolicy, TheFileGoesOnlyAfterTheSendOrItsStagedCopy) {
  EXPECT_EQ(DecideVoiceFileFate(/*sent=*/true, 0, 0), VoiceFileFate::Delete);
  EXPECT_EQ(DecideVoiceFileFate(false, 0, 1), VoiceFileFate::Delete);       // failed after staging: Retry has the copy
  EXPECT_EQ(DecideVoiceFileFate(false, 2, 3), VoiceFileFate::Delete);
}

TEST(VoiceSendPolicy, ARefusalBeforeStagingKeepsTheRecording) {
  EXPECT_EQ(DecideVoiceFileFate(false, 0, 0), VoiceFileFate::KeepForRetry); // the send-rate limit, a failed prepare
  EXPECT_EQ(DecideVoiceFileFate(false, 2, 2), VoiceFileFate::KeepForRetry); // an older staged send is not this one
}

} // namespace
} // namespace pbr
