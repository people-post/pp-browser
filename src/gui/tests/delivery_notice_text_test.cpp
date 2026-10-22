// Delivery notices worded in the user's language, and the transcript's receive-failure line.
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"

#include "common/ValueJson.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

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
  DeliveryNotice relay;
  relay.kind = DeliveryNotice::Kind::SentViaRelay;
  EXPECT_EQ(relay.kind, DeliveryNotice::Kind::SentViaRelay);
  EXPECT_EQ(DeliveryNoticeText(relay).find("chat.notice"), std::string::npos);
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
  EXPECT_EQ(DeliveryNoticeText(notice), "Couldn't read a message from a peer (couldn't decrypt).");
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
  EXPECT_NE(std::string(rows[0].content_rml.c_str()).find("Couldn't read a message from account:lHaE… (bad signature)."),
            std::string::npos)
      << rows[0].content_rml.c_str();
}

} // namespace
} // namespace pbr
