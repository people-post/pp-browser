// #210: a tap on a message's red mark says why it did not go out — the reason its stored "did not go out" line keeps —
// else one plain sentence. Nothing is sent again.
#include "feature/presenters/DeliveryNoticePlan.h"
#include "feature/presenters/SendMark.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

namespace pbr {
namespace {

ThreadMessage StoredLine(const SendFailure failure) {
  DeliveryNotice notice;
  notice.kind = DeliveryNotice::Kind::SendFailed;
  notice.send = failure;
  notice.thread_id = "t1";
  notice.message_id = "m1";
  const DeliveryNoticePlan plan = PlanDeliveryNotice(notice, 1000);
  EXPECT_TRUE(plan.row);
  return plan.row ? *plan.row : ThreadMessage{};
}

TEST(SendFailureReasonTest, TheStoredLinesReasonIsTold) {
  const ThreadMessage line = StoredLine(SendFailure::RelayUnreachable);
  EXPECT_EQ(line.id, SendFailureRowId("m1", "")) << "the row the tap looks up";
  EXPECT_EQ(SendFailureReason(line, false), SendFailure::RelayUnreachable);
  EXPECT_EQ(SendFailureReason(StoredLine(SendFailure::PeerNoAnswer), false), SendFailure::PeerNoAnswer);
}

TEST(SendFailureReasonTest, WithoutAKnownReasonTheGenericSentence) {
  EXPECT_FALSE(SendFailureReason(std::nullopt, false)) << "no line: failed before it was sent, or a restart";
  ThreadMessage unknown = MakeSendFailureRow("t1", "m1", "no_such_reason", 1);
  EXPECT_FALSE(SendFailureReason(unknown, false));
  ThreadMessage unknown_word = MakeSendFailureRow("t1", "m1", SendFailureWord(SendFailure::Unknown).c_str(), 1);
  EXPECT_FALSE(SendFailureReason(unknown_word, false));
  ThreadMessage not_a_line;
  not_a_line.id = SendFailureRowId("m1", "");
  not_a_line.content_type = ChatContentType::Text;
  not_a_line.text = "relay_unreachable";
  EXPECT_FALSE(SendFailureReason(not_a_line, false)) << "only our stored line counts";
  EXPECT_FALSE(SendFailureReason(MakeSendFailureRow("t1", "m1", kGroupMembersMissedWire, 1), false));
}

TEST(SendFailureReasonTest, AGroupMessageGetsTheGenericSentence) {
  EXPECT_FALSE(SendFailureReason(StoredLine(SendFailure::RelayUnreachable), true));
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

TEST(SendFailureReasonTest, BothLanguagesHaveTheWords) {
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const char* key :
         {"chat.send.not_sent", "chat.send.mark.queued", "chat.send.mark.failed", "chat.notice.send.relay_busy",
          "chat.notice.send.relay_unreachable", "chat.notice.send.direct_unconfirmed"}) {
      const std::string text = Tr(key);
      EXPECT_FALSE(text.empty()) << language << " " << key;
      EXPECT_NE(text, key) << language << ": missing " << key;
    }
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
  EXPECT_EQ(Tr("chat.send.not_sent"), "This message wasn't sent. Please send it again.");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(Tr("chat.send.not_sent"), "这条消息没有发出去，请重新发送。");
  LocalizationService::Instance().SetPreferredLanguage("en");
  // The three reasons that promised a retry no longer do: the red mark shows when PP has stopped trying.
  const std::string en = ReadFile(std::string(PP_BROWSER_ASSETS_DIR) + "/locales/en.json");
  const std::string zh = ReadFile(std::string(PP_BROWSER_ASSETS_DIR) + "/locales/zh-Hans.json");
  ASSERT_FALSE(en.empty());
  ASSERT_FALSE(zh.empty());
  EXPECT_EQ(en.find("The message server is busy. PP will try again"), std::string::npos);
  EXPECT_EQ(en.find("check your connection. PP will try again"), std::string::npos);
  EXPECT_EQ(en.find("PP will try to deliver it through the message server"), std::string::npos);
  EXPECT_EQ(zh.find("消息稍后会重试"), std::string::npos);
  EXPECT_EQ(zh.find("请检查网络；稍后会重试"), std::string::npos);
  EXPECT_EQ(zh.find("将尽可能通过服务器中继送达"), std::string::npos);
}

} // namespace
} // namespace pbr
