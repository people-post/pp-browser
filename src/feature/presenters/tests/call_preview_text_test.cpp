#include "feature/presenters/GroupEventText.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/calls/CallControlCodec.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// The chat list's preview of a call summary row is the transcript's icon and short words in the UI language; the stored stand-in text
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
  auto message = CallControlCodec::BuildSystemMessage("thread-1", CallControlType::CallSummary,
                                                      media == CallMediaMode::Video ? "Video call" : "Voice call",
                                                      *encoded, sender);
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

std::string TextOf(const ThreadMessage& message) {
  const auto preview = StoredCallPreview(message);
  return preview ? preview->text : std::string("<none>");
}

std::string IconOf(const ThreadMessage& message) {
  const auto preview = StoredCallPreview(message);
  return preview ? preview->icon : std::string("<none>");
}

TEST_F(CallPreviewTextTest, ACompletedCallIsItsDirectionIconAndTheDurationOnly) {
  const auto out = Summary(CallMediaMode::Voice, CallOutcome::Completed, kLocalSelfContactId, 65000);
  EXPECT_EQ(TextOf(out), "1:05");
  EXPECT_EQ(IconOf(out), "phone-outgoing");
  const auto in = Summary(CallMediaMode::Voice, CallOutcome::Completed, "peer", 49000);
  EXPECT_EQ(TextOf(in), "0:49");
  EXPECT_EQ(IconOf(in), "phone-incoming");
  // Video keeps the phone icons and says "视频" first.
  const auto video = Summary(CallMediaMode::Video, CallOutcome::Completed, "peer", 5000);
  EXPECT_EQ(TextOf(video), "视频 · 0:05");
  EXPECT_EQ(IconOf(video), "phone-incoming");
}

TEST_F(CallPreviewTextTest, MissedAndDeclinedCallsAreTheirIconAndOneShortWord) {
  EXPECT_EQ(TextOf(Summary(CallMediaMode::Voice, CallOutcome::Missed, "peer")), "未接");
  EXPECT_EQ(IconOf(Summary(CallMediaMode::Voice, CallOutcome::Missed, "peer")), "phone-missed");
  EXPECT_EQ(TextOf(Summary(CallMediaMode::Video, CallOutcome::Declined, "peer")), "视频 · 已拒绝");
  EXPECT_EQ(IconOf(Summary(CallMediaMode::Video, CallOutcome::Declined, "peer")), "phone-declined");
  const auto by_peer = Summary(CallMediaMode::Voice, CallOutcome::Declined, kLocalSelfContactId);
  EXPECT_EQ(TextOf(by_peer), "对方已拒绝");
  EXPECT_EQ(IconOf(by_peer), "phone-declined");
}

// Every state x voice / video x my side / their side maps to one of the four icons and a short label; the icon never
// depends on voice vs video, and the label is the same for both apart from the "视频" lead.
TEST_F(CallPreviewTextTest, EveryStateMapsToAnIconAndAShortLabel) {
  using Outcome = CallSummaryRow::Outcome;
  struct Case {
    Outcome outcome;
    bool ours;
    const char* icon;
    const char* label;
  };
  const Case cases[] = {
      {Outcome::Connected, true, "phone-outgoing", "1:05"},
      {Outcome::Connected, false, "phone-incoming", "1:05"},
      {Outcome::NoAnswer, true, "phone-outgoing", "无人接听"},
      {Outcome::NoAnswer, false, "phone-outgoing", "无人接听"},
      {Outcome::Cancelled, true, "phone-outgoing", "已取消"},
      {Outcome::Cancelled, false, "phone-outgoing", "已取消"},
      {Outcome::Missed, true, "phone-missed", "未接"},
      {Outcome::Missed, false, "phone-missed", "未接"},
      {Outcome::Declined, true, "phone-declined", "已拒绝"},
      {Outcome::Declined, false, "phone-declined", "已拒绝"},
      {Outcome::DeclinedByPeer, true, "phone-declined", "对方已拒绝"},
      {Outcome::DeclinedByPeer, false, "phone-declined", "对方已拒绝"},
      {Outcome::Failed, true, "phone-declined", "未接通"},
      {Outcome::Failed, false, "phone-declined", "未接通"},
  };
  for (const Case& c : cases) {
    for (const bool video : {false, true}) {
      CallSummaryRow row;
      row.outcome = c.outcome;
      row.ours = c.ours;
      row.video = video;
      row.connected = c.outcome == Outcome::Connected;
      row.duration = row.connected ? "1:05" : "";
      const CallRowLook look = CallRowLookOf(row);
      EXPECT_EQ(look.icon, c.icon) << c.label << " video=" << video << " ours=" << c.ours;
      EXPECT_EQ(look.text, std::string(video ? "视频 · " : "") + c.label)
          << c.label << " video=" << video << " ours=" << c.ours;
      row.group = true;
      EXPECT_EQ(CallRowLookOf(row).text, std::string("群通话 · ") + (video ? "视频 · " : "") + c.label);
    }
  }
}

TEST_F(CallPreviewTextTest, ASummaryThatDoesNotDecodeFallsBackToTheCallTitle) {
  ThreadMessage broken;
  broken.content_type = ChatContentType::System;
  broken.text = "Voice call";
  EXPECT_EQ(TextOf(broken), "语音通话");
  EXPECT_EQ(IconOf(broken), "");
  broken.text = "Video call";
  EXPECT_EQ(TextOf(broken), "视频通话");
}

TEST_F(CallPreviewTextTest, APersonsOwnWordsAreLeftAlone) {
  ThreadMessage typed;
  typed.content_type = ChatContentType::Text;
  typed.text = "Voice call";
  EXPECT_FALSE(StoredCallPreview(typed));
  ThreadMessage other;
  other.content_type = ChatContentType::System;
  other.text = "Chat key updated.";
  EXPECT_FALSE(StoredCallPreview(other));
}

} // namespace
} // namespace pbr
