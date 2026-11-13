// #210: our message shows a clock while it is being sent, a red mark when it did not go out (a tap says why), nothing
// once it is out — on text, pictures and files, voice and cards alike (DEV/PLAN-message-send-status.md).
#include "feature/conversations/InboxController.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <gtest/gtest.h>

#include <cctype>
#include <map>

namespace pbr {
namespace {

TranscriptRow Ours(const std::string& id, const ChatContentType type) {
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = kLocalSelfContactId;
  row.message.content_type = type;
  row.message.text = "hi";
  row.message.timestamp = 1;
  return row;
}

TranscriptRow Voice(const std::string& id) {
  TranscriptRow row = Ours(id, ChatContentType::Attachment);
  ChatAttachmentFields fields;
  fields.mime = "audio/ogg";
  fields.filename = "voice-4200.ogg";
  fields.byte_length = 25000;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, 1);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  row.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  return row;
}

/** Facts whose send marks come from `marks` (by message id; none when not listed). */
TranscriptFacts FactsWith(const std::map<std::string, SendMark>& marks) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/voice.ogg";
    return view;
  };
  facts.voice_supported = true;
  facts.voice_max_seconds = 120;
  facts.send_mark = [marks](const ThreadMessage& message) {
    const auto it = marks.find(message.id);
    return it == marks.end() ? SendMark::None : it->second;
  };
  return facts;
}

class TranscriptSendMarkTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
  void TearDown() override { LocalizationService::Instance().SetPreferredLanguage("en"); }
};

TEST_F(TranscriptSendMarkTest, EveryKindOfOurMessageCarriesItsMarkBeforeItsBubble) {
  const std::vector<TranscriptRow> rows_in = {Ours("text", ChatContentType::Text),
                                              Ours("file", ChatContentType::Attachment), Voice("voice"),
                                              Ours("card", ChatContentType::ContactCard)};
  for (const SendMark mark : {SendMark::Queued, SendMark::Failed}) {
    std::map<std::string, SendMark> marks;
    for (const TranscriptRow& row : rows_in) {
      marks[row.message.id] = mark;
    }
    const auto rows = RenderTranscript(rows_in, FactsWith(marks));
    ASSERT_EQ(rows.size(), rows_in.size());
    for (const MessageDisplayRow& row : rows) {
      const std::string id = row.message_id.c_str();
      const std::string rml = row.content_rml.c_str();
      EXPECT_EQ(row.send_mark, mark == SendMark::Queued ? 1 : 2) << id;
      const size_t mark_at = rml.find("class=\"send-mark ");
      ASSERT_NE(mark_at, std::string::npos) << id << ": " << rml;
      size_t body_at = rml.find("<div class=\"bubble ");
      if (body_at == std::string::npos) {
        body_at = rml.find("<div class=\"chat-card");
      }
      ASSERT_NE(body_at, std::string::npos) << id << ": " << rml;
      EXPECT_LT(mark_at, body_at) << id << ": left of the bubble";
      if (mark == SendMark::Failed) {
        EXPECT_NE(rml.find("<button class=\"send-mark send-mark--failed\""), std::string::npos) << id;
        EXPECT_NE(rml.find("show_send_failure('" + id + "')"), std::string::npos) << id << ": " << rml;
        EXPECT_NE(rml.find("alert-circle.svg"), std::string::npos) << id;
      } else {
        EXPECT_NE(rml.find("send-mark--queued"), std::string::npos) << id;
        EXPECT_NE(rml.find("clock.svg"), std::string::npos) << id;
        EXPECT_EQ(rml.find("show_send_failure"), std::string::npos) << id << ": a clock is not tappable";
        EXPECT_EQ(rml.find("<button class=\"send-mark"), std::string::npos) << id;
      }
    }
  }
  EXPECT_NE(
      std::string(RenderTranscript({Voice("voice")}, FactsWith({{"voice", SendMark::Failed}}))[0].content_rml.c_str())
          .find("chat-voice-card"),
      std::string::npos)
      << "the voice row is drawn as a voice bubble";
}

TEST_F(TranscriptSendMarkTest, ASentMessageAndOneWithNoMarkHaveNoMarkNode) {
  const auto rows = RenderTranscript({Ours("m1", ChatContentType::Text)}, FactsWith({{"m1", SendMark::None}}));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].send_mark, 0);
  EXPECT_EQ(std::string(rows[0].content_rml.c_str()).find("send-mark"), std::string::npos);
  const auto unset = RenderTranscript({Ours("m1", ChatContentType::Text)}, TranscriptFacts{});
  EXPECT_EQ(std::string(unset[0].content_rml.c_str()).find("send-mark"), std::string::npos)
      << "no send_mark port: nothing";
}

TEST_F(TranscriptSendMarkTest, TheTapArgumentStaysInsideItsAttribute) {
  const auto rows =
      RenderTranscript({Ours("m'\"<x>", ChatContentType::Text)}, FactsWith({{"m'\"<x>", SendMark::Failed}}));
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_EQ(rml.find("<x>"), std::string::npos) << rml;
  EXPECT_NE(rml.find("show_send_failure('m\\'&quot;&lt;x&gt;')"), std::string::npos) << rml;
}

TEST_F(TranscriptSendMarkTest, TheTailStillHangsOnTheBubbleOfAMarkedMessage) {
  const auto rows = RenderTranscript({Ours("m1", ChatContentType::Text)}, FactsWith({{"m1", SendMark::Failed}}));
  ASSERT_EQ(rows.size(), 1u);
  const std::string rml = rows[0].content_rml.c_str();
  EXPECT_TRUE(rows[0].run_end);
  EXPECT_NE(rml.find("bubble-tail-out"), std::string::npos) << rml;
  EXPECT_LT(rml.find("send-mark"), rml.find("bubble-tail-host"));
}

TEST_F(TranscriptSendMarkTest, ATapSaysTheStoredReasonElseTheGenericSentence) {
  EXPECT_EQ(SendFailureReasonText(SendFailure::RelayUnreachable),
            "Couldn't reach the server. This message wasn't sent — check your connection, then send it again.");
  EXPECT_EQ(SendFailureReasonText(std::nullopt), "This message wasn't sent. Please send it again.");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(SendFailureReasonText(SendFailure::RelayUnreachable),
            "服务器现在连不上，这条消息没有发出去。请检查网络后重新发送。");
  EXPECT_EQ(SendFailureReasonText(std::nullopt), "这条消息没有发出去，请重新发送。");
}

// Every reason the pill (and a kept grey line) can show: it says the message was not sent, never that PP will retry,
// and in plain words — no protocol terms.
TEST_F(TranscriptSendMarkTest, NoReasonPromisesARetryAnyMore) {
  const auto lower = [](std::string text) {
    for (char& c : text) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
  };
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (int i = static_cast<int>(SendFailure::Unknown); i <= static_cast<int>(SendFailure::SenderNotRegistered); ++i) {
      const auto failure = static_cast<SendFailure>(i);
      const std::string text = lower(SendFailureText(failure));
      const std::string what = std::string(language) + " " + SendFailureWord(failure).c_str() + ": " + text;
      EXPECT_EQ(text.find("chat.notice"), std::string::npos) << what;
      for (const char* promise : {"try again", "will try", "pp will", "shortly", "deliver it", "重试", "送达", "稍后会",
                                  "再尝试", "稍后再试", "会自动"}) {
        EXPECT_EQ(text.find(promise), std::string::npos) << what << " (promises: " << promise << ")";
      }
      for (const char* term : {"中继", "relay", "peer", "节点", "签名"}) {
        EXPECT_EQ(text.find(term), std::string::npos) << what << " (protocol word: " << term << ")";
      }
      EXPECT_TRUE(text.find("wasn't sent") != std::string::npos || text.find("没有发出去") != std::string::npos)
          << what << " (must say it was not sent)";
    }
  }
}

// #210 step 2: what a tap that does not send again says.
TEST_F(TranscriptSendMarkTest, ThePillsWhenATapDoesNotSendAgain) {
  using Refusal = SendMarkTap::Refusal;
  EXPECT_EQ(SendFailureReasonText(std::nullopt, Refusal::NotText),
            "This message wasn't sent. Pictures, files, voice messages and cards can't be re-sent with a tap yet. "
            "Please send it again.");
  EXPECT_EQ(SendFailureReasonText(SendFailure::RelayBusy, Refusal::NotText), SendFailureText(SendFailure::RelayBusy));
  EXPECT_EQ(SendFailureReasonText(std::nullopt, Refusal::ThisKind),
            "This message can't be re-sent with a tap. Please send it again.");
  EXPECT_EQ(
      SendFailureReasonText(SendFailure::RelayBusy, Refusal::ThisKind),
      SendFailureText(SendFailure::RelayBusy) + " This message can't be re-sent with a tap. Please send it again.");
  EXPECT_EQ(LocalizationService::Instance().Tr("chat.send.still_trying"), "Still trying to send. Please wait.");
  LocalizationService::Instance().SetPreferredLanguage("zh-Hans");
  EXPECT_EQ(SendFailureReasonText(std::nullopt, Refusal::ThisKind), "这条消息这次不能点一下重发，请重新发送。");
  EXPECT_EQ(SendFailureReasonText(SendFailure::RelayBusy, Refusal::ThisKind),
            SendFailureText(SendFailure::RelayBusy) + "这条消息这次不能点一下重发，请重新发送。");
  EXPECT_EQ(SendFailureReasonText(std::nullopt, Refusal::NotText),
            "这条消息没有发出去。图片、文件、语音和名片暂时不能点一下重发，请重新发送。");
  EXPECT_EQ(LocalizationService::Instance().Tr("chat.send.still_trying"), "还在尝试发送，请稍等。");
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    for (const char* key : {"chat.send.still_trying", "chat.send.not_resendable", "chat.send.not_sent_other"}) {
      std::string text = LocalizationService::Instance().Tr(key);
      for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      for (const char* term : {"中继", "relay", "peer", "节点", "签名"}) {
        EXPECT_EQ(text.find(term), std::string::npos) << language << " " << key << ": " << text;
      }
    }
  }
}

// A 1:1 "did not go out" line goes only when its message shows the red mark (or is sent: TranscriptAssembly);
// otherwise it is the only sign of the failure and stays.
TEST_F(TranscriptSendMarkTest, AFailureLineGoesOnlyWhenItsMessageShowsTheRedMark) {
  const auto ids = [](const std::vector<MessageDisplayRow>& rows) {
    std::vector<std::string> out;
    for (const MessageDisplayRow& row : rows) {
      out.push_back(row.message_id.c_str());
    }
    return out;
  };
  const std::string why = SendFailureWord(SendFailure::RelayUnreachable).c_str();
  ThreadMessage text = Ours("m1", ChatContentType::Text).message;
  text.delivery = MessageDelivery::Failed;
  const ThreadMessage text_line = MakeSendFailureRow("t", "m1", why, 2);

  // A failed text: the mark shows, the line goes.
  auto rows = RenderTranscript(AssembleTranscript({text, text_line}, nullptr, ThreadKind::Direct),
                               FactsWith({{"m1", SendMark::Failed}}));
  ASSERT_EQ(ids(rows), (std::vector<std::string>{"m1"}));
  EXPECT_EQ(rows[0].send_mark, 2);

  // Still a clock, or no mark port at all: the line stays.
  rows = RenderTranscript(AssembleTranscript({text, text_line}, nullptr, ThreadKind::Direct),
                          FactsWith({{"m1", SendMark::Queued}}));
  EXPECT_EQ(ids(rows), (std::vector<std::string>{"m1", "send-failure-m1"}));
  rows = RenderTranscript(AssembleTranscript({text, text_line}, nullptr, ThreadKind::Direct), TranscriptFacts{});
  EXPECT_EQ(ids(rows), (std::vector<std::string>{"m1", "send-failure-m1"}));

  // A failed reaction (an annotation carries no mark): its line stays and says why.
  ThreadMessage peer = Ours("p1", ChatContentType::Text).message;
  peer.sender_contact_id = "account:bob";
  peer.delivery = MessageDelivery::Relayed;
  ThreadMessage reaction = Ours("r1", ChatContentType::Annotation).message;
  reaction.text = "\xF0\x9F\x91\x8D";
  reaction.target_message_id = "p1";
  reaction.payload_json = BuildReactionPayloadJson(kAnnotationTypeReaction, "p1", reaction.text);
  reaction.delivery = MessageDelivery::Failed;
  const ThreadMessage reaction_line = MakeSendFailureRow("t", "r1", why, 3);
  rows = RenderTranscript(AssembleTranscript({peer, reaction, reaction_line}, nullptr, ThreadKind::Direct),
                          FactsWith({{"r1", SendMark::Failed}}));
  ASSERT_EQ(ids(rows), (std::vector<std::string>{"p1", "send-failure-r1"}));
  EXPECT_NE(std::string(rows[1].content_rml.c_str()).find("Couldn't reach the server"), std::string::npos)
      << rows[1].content_rml.c_str();
}

} // namespace
} // namespace pbr
