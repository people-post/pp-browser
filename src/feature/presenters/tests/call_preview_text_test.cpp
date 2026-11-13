#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/calls/CallControlCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The chat list's preview of a call summary row is the transcript's line in the UI language; the stored stand-in text
// ("Voice call") is English and is not rewritten.

ThreadMessage Summary(CallMediaMode media, CallOutcome outcome, const std::string& sender,
                      std::optional<int64_t> duration_ms = std::nullopt) {
  CallSummaryDetail detail;
  detail.call_id = "call-1";
  detail.media_mode = media;
  detail.outcome = outcome;
  detail.duration_ms = duration_ms;
  auto encoded = CallControlCodec::EncodeSummary(detail);
  EXPECT_TRUE(static_cast<bool>(encoded));
  auto message = CallControlCodec::BuildSystemMessage(
      "thread-1", CallControlType::CallSummary, media == CallMediaMode::Video ? "Video call" : "Voice call", *encoded,
      sender);
  EXPECT_TRUE(static_cast<bool>(message));
  message->sender_contact_id = sender;
  return *message;
}

class CallPreviewTextTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(CallPreviewTextTest, OnlyTheStandInTextIsACallPreview) {
  EXPECT_TRUE(IsStoredCallPreview("Voice call"));
  EXPECT_TRUE(IsStoredCallPreview("Video call"));
  EXPECT_FALSE(IsStoredCallPreview("voice call"));
  EXPECT_FALSE(IsStoredCallPreview("Voice call tonight?"));
}

TEST_F(CallPreviewTextTest, ACompletedCallReadsAsTheTranscriptLineWithItsDuration) {
  const auto out = StoredCallPreviewText(Summary(CallMediaMode::Voice, CallOutcome::Completed, kLocalSelfContactId, 65000));
  ASSERT_TRUE(out);
  EXPECT_EQ(*out, "呼出语音通话 · 1:05");
  const auto in = StoredCallPreviewText(Summary(CallMediaMode::Video, CallOutcome::Completed, "peer", 5000));
  ASSERT_TRUE(in);
  EXPECT_EQ(*in, "呼入视频通话 · 0:05");
}

TEST_F(CallPreviewTextTest, MissedAndDeclinedCallsFollowTheTranscriptLine) {
  EXPECT_EQ(StoredCallPreviewText(Summary(CallMediaMode::Voice, CallOutcome::Missed, "peer")).value_or(""),
            "未接语音通话");
  EXPECT_EQ(StoredCallPreviewText(Summary(CallMediaMode::Video, CallOutcome::Declined, "peer")).value_or(""),
            "已拒绝视频通话");
  EXPECT_EQ(StoredCallPreviewText(Summary(CallMediaMode::Voice, CallOutcome::Declined, kLocalSelfContactId)).value_or(""),
            "语音通话 · 对方已拒绝");
}

TEST_F(CallPreviewTextTest, ASummaryThatDoesNotDecodeFallsBackToTheCallTitle) {
  ThreadMessage broken;
  broken.content_type = ChatContentType::System;
  broken.text = "Voice call";
  EXPECT_EQ(StoredCallPreviewText(broken).value_or(""), "语音通话");
  broken.text = "Video call";
  EXPECT_EQ(StoredCallPreviewText(broken).value_or(""), "视频通话");
}

TEST_F(CallPreviewTextTest, APersonsOwnWordsAreLeftAlone) {
  ThreadMessage typed;
  typed.content_type = ChatContentType::Text;
  typed.text = "Voice call";
  EXPECT_FALSE(StoredCallPreviewText(typed));
  ThreadMessage other;
  other.content_type = ChatContentType::System;
  other.text = "Chat key updated.";
  EXPECT_FALSE(StoredCallPreviewText(other));
}

} // namespace
} // namespace pbr
