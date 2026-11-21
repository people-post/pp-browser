#include "gui/chat/VoiceSendPolicy.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

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

// What a voice send's end does to its recording and bubble. A "not staged" end while the chat's staged count grew
// (another send staged meanwhile) deletes the file and removes the bubble: it is never left "Uploading" (F3).
TEST(VoiceSendPolicy, EveryVoiceSendEndSettlesItsFileAndBubble) {
  using O = AttachmentSendOutcome;
  EXPECT_EQ(DecideVoiceSendSettle(O::Sent, true, 0, 0), VoiceSendSettle::DeleteFile);
  EXPECT_EQ(DecideVoiceSendSettle(O::FailedStaged, false, 0, 1), VoiceSendSettle::DeleteFile);
  EXPECT_EQ(DecideVoiceSendSettle(O::FailedNotStaged, false, 0, 0), VoiceSendSettle::KeepBehindRetry);
  EXPECT_EQ(DecideVoiceSendSettle(O::FailedNotStaged, false, 1, 2), VoiceSendSettle::DeleteFileRemoveBubble);
  EXPECT_EQ(DecideVoiceSendSettle(O::RateLimited, false, 0, 0), VoiceSendSettle::QueueKeeps);
  EXPECT_EQ(DecideVoiceSendSettle(O::RateLimited, false, 1, 2), VoiceSendSettle::QueueKeeps)
      << "refused before staging: the job keeps its file whatever else was staged";
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

// The keyboard key leaves voice mode and the choice is remembered; reopening a chat restores it. (The controller is wired
// to the whole app, so these are source pins; the decisions themselves are tested above and in voice_composer_test.)
TEST(VoiceSendPolicy, TheKeyboardKeyIsTheOnlyWayOutAndItIsRemembered) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / "src/gui/chat/ChatController.cpp");
  std::stringstream text;
  text << in.rdbuf();
  const std::string source = text.str();
  const size_t toggle = source.find("void ChatController::ToggleVoiceMode()");
  ASSERT_NE(toggle, std::string::npos);
  const std::string body = source.substr(toggle, 1400);
  EXPECT_NE(body.find("chat_.voice_mode = !chat_.voice_mode;"), std::string::npos);
  EXPECT_NE(body.find("prefs.composer_voice_mode_last = chat_.voice_mode;"), std::string::npos)
      << "remembered on a tap";
  EXPECT_NE(source.find("RestoreVoiceMode(Store().Snapshot().profile_prefs.composer_voice_mode_last"),
            std::string::npos)
      << "a reopened chat restores it";
  // A send in progress never disables the bar: voice messages queue (VoiceSendQueue); `eligible` only leaves voice mode.
  const size_t eligible = source.find("voice_ports.eligible = [this]() {");
  ASSERT_NE(eligible, std::string::npos);
  const std::string eligible_body = source.substr(eligible, source.find("};", eligible) - eligible);
  EXPECT_EQ(eligible_body.find("AttachmentBusy()"), std::string::npos);
  EXPECT_EQ(source.find("voice_ports.busy"), std::string::npos)
      << "no busy port: the bar is never refused for an upload";
  EXPECT_NE(source.find("voice_queue_ports.slot_free = [this]() {"), std::string::npos);
}

} // namespace
} // namespace pbr
