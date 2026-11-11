// Delivery notices worded in the user's language, and the transcript's receive-failure line.
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

struct RenderedRow {
  std::string rml;
};

class DeliveryNoticeTextTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(DeliveryNoticeTextTest, EveryKindHasWords) {
  for (int i = static_cast<int>(SendFailure::Unknown); i <= static_cast<int>(SendFailure::RelayRejected); ++i) {
    DeliveryNotice notice;
    notice.send = static_cast<SendFailure>(i);
    const std::string text = DeliveryNoticeText(notice);
    EXPECT_FALSE(text.empty());
    EXPECT_EQ(text.find("chat.notice"), std::string::npos) << "a missing key shows itself: " << text;
  }
  DeliveryNotice group;
  group.kind = DeliveryNotice::Kind::GroupMembersMissed;
  EXPECT_EQ(DeliveryNoticeText(group), "Some group members couldn't receive this message.");
}

TEST_F(DeliveryNoticeTextTest, ASentViaRelayToastIsGone) {
  EXPECT_EQ(LocalizationService::Instance().Tr("chat.notice.sent_via_relay"), "chat.notice.sent_via_relay")
      << "the key answers with itself: it was removed with the toast";
}

TEST_F(DeliveryNoticeTextTest, AReceiveFailureNamesThePeerAndTheReason) {
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::ReceiveFailed;
  notice.receive = ReceiveFailure::ApplyFailed;
  notice.peer = "account:lHaEnkO4vSbVnl6A";
  notice.family = "call";
  EXPECT_EQ(DeliveryNoticeText(notice), "Couldn't read a message from account:lHaE… (couldn't apply call update).");
  notice.receive = ReceiveFailure::DecryptFailed;
  notice.peer.clear();
  EXPECT_EQ(DeliveryNoticeText(notice), "Couldn't read a message from a contact (couldn't decrypt).");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(DeliveryNoticeText(notice).find("Couldn't"), std::string::npos) << "in the reader's language";
}

TEST_F(DeliveryNoticeTextTest, TheTranscriptWordsAStoredReceiveFailureLine) {
  Object detail;
  detail.set("sender", "account:lHaEnkO4vSbVnl6A");
  detail.set("reason", "bad_signature");
  detail.set("family", "");
  detail.set("detail", "Envelope signature did not verify");
  Object payload;
  payload.set("control_type", "receive_failure");
  payload.set("detail", DumpJson(detail));
  TranscriptRow row;
  row.message.id = "local-1";
  row.message.sender_contact_id = kLocalSelfContactId;
  row.message.content_type = ChatContentType::System;
  row.message.payload_json = DumpJson(payload);
  auto rows = RenderTranscript({row}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NE(
      std::string(rows[0].content_rml.c_str()).find("Couldn't read a message from account:lHaE… (bad signature)."),
      std::string::npos)
      << rows[0].content_rml.c_str();
}

RenderedRow SystemRowOf(const ThreadMessage& message) {
  TranscriptRow row;
  row.message = message;
  auto rows = RenderTranscript({row}, TranscriptFacts{});
  EXPECT_EQ(rows.size(), 1u);
  return RenderedRow{rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str())};
}

// DEV/PLAN-call-stranger-declined.md (c): the dropped calls' line, a grey system line worded from its count.
TEST_F(DeliveryNoticeTextTest, TheTranscriptWordsABlockedCallsLine) {
  Object detail;
  detail.set("count", static_cast<int64_t>(1));
  detail.set("setting", "contacts_only");
  Object payload;
  payload.set("control_type", "call_blocked");
  payload.set("detail", DumpJson(detail));
  ThreadMessage line;
  line.id = "call-blocked:account:stranger";
  line.sender_contact_id = kLocalSelfContactId;
  line.content_type = ChatContentType::System;
  line.payload_json = DumpJson(payload);
  const std::string one = SystemRowOf(line).rml;
  EXPECT_NE(one.find("chat-system-line muted"), std::string::npos) << one;
  EXPECT_NE(one.find("Missed call (declined by your settings)"), std::string::npos) << one;

  detail.set("count", static_cast<int64_t>(3));
  payload.set("detail", DumpJson(detail));
  line.payload_json = DumpJson(payload);
  EXPECT_NE(SystemRowOf(line).rml.find("3 missed calls (declined by your settings)"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_NE(SystemRowOf(line).rml.find("3 次未接来电（已按你的设置拒接）"), std::string::npos) << SystemRowOf(line).rml;
}

TEST_F(DeliveryNoticeTextTest, TheTranscriptWordsASendFailureLineUnderItsMessage) {
  const ThreadMessage failed =
      MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::RelayUnreachable).c_str(), 1);
  EXPECT_NE(SystemRowOf(failed).rml.find("Couldn't reach the message server"), std::string::npos)
      << SystemRowOf(failed).rml;
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  const std::string zh = SystemRowOf(failed).rml;
  EXPECT_EQ(zh.find("Couldn't"), std::string::npos) << "in the reader's language: " << zh;
  EXPECT_NE(zh.find("服务器"), std::string::npos) << zh;

  const ThreadMessage group = MakeSendFailureRow("g1", "m2", kGroupMembersMissedWire, 1);
  EXPECT_EQ(SystemRowOf(group).rml.find("Some group members"), std::string::npos);
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_NE(SystemRowOf(group).rml.find("Some group members couldn't receive this message."), std::string::npos);
}

TEST_F(DeliveryNoticeTextTest, ASystemLineStoredInEnglishReadsInTheUiLanguage) {
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  for (const char* stored : {"You blocked the inviter", "Noted — you can keep chatting with people who are online.",
                             "Chat key updated.", "New messages stay on this device.", "Accepted group invitation"}) {
    ThreadMessage message;
    message.id = "x";
    message.sender_contact_id = kLocalSelfContactId;
    message.content_type = ChatContentType::System;
    message.text = stored;
    const std::string rml = SystemRowOf(message).rml;
    EXPECT_EQ(rml.find(stored), std::string::npos) << stored << " -> " << rml;
  }
  ThreadMessage joined;
  joined.id = "y";
  joined.content_type = ChatContentType::System;
  joined.text = "You joined Book club";
  const std::string rml = SystemRowOf(joined).rml;
  EXPECT_EQ(rml.find("You joined"), std::string::npos) << rml;
  EXPECT_NE(rml.find("Book club"), std::string::npos) << rml;
}

TEST_F(DeliveryNoticeTextTest, APersonsOwnWordsAreNotMapped) {
  ThreadMessage typed;
  typed.id = "z";
  typed.sender_contact_id = "someone";
  typed.content_type = ChatContentType::Text;
  typed.text = "Chat key updated.";
  TranscriptRow row;
  row.message = typed;
  auto rows = RenderTranscript({row}, TranscriptFacts{});
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_NE(std::string(rows[0].content_rml.c_str()).find("Chat key updated."), std::string::npos);
}

} // namespace
} // namespace pbr
