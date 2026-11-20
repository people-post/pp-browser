// Each ordinary message shows how it travelled as a small icon right before its time: direct or encrypted relay.
#include "feature/conversations/InboxController.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/chat/TranscriptRml.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <gtest/gtest.h>

#include <map>

namespace pbr {
namespace {

constexpr const char* kDirect = "route-direct.svg";
constexpr const char* kRelay = "route-relay.svg";

TranscriptRow Message(const std::string& id, const bool ours, const std::optional<MessageTransport> transport) {
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = ours ? kLocalSelfContactId : "account:peer";
  row.message.content_type = ChatContentType::Text;
  row.message.text = "hi";
  row.message.timestamp = 1'700'000'000'000;
  row.message.transport = transport;
  return row;
}

TranscriptRow Voice(const std::string& id, const bool ours, const std::optional<MessageTransport> transport) {
  TranscriptRow row = Message(id, ours, transport);
  row.message.content_type = ChatContentType::Attachment;
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

TranscriptFacts FactsWith(const std::map<std::string, SendMark>& marks = {}) {
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

std::string RmlOf(const TranscriptRow& row, const TranscriptFacts& facts) {
  const auto rows = RenderTranscript({row}, facts);
  EXPECT_EQ(rows.size(), 1u);
  return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
}

class TranscriptRouteIconTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
  }
};

TEST_F(TranscriptRouteIconTest, DirectAndRelayShowTheirIconBeforeTheTime) {
  for (const bool ours : {false, true}) {
    const std::string direct = RmlOf(Message("m", ours, MessageTransport::Direct), FactsWith());
    const size_t time_at = direct.find("<div class=\"bubble-time\">");
    ASSERT_NE(time_at, std::string::npos) << direct;
    const size_t icon_at = direct.find(kDirect);
    ASSERT_NE(icon_at, std::string::npos) << direct;
    EXPECT_GT(icon_at, time_at) << "inside the time line";
    EXPECT_LT(icon_at, direct.find("</svg>", icon_at) + 1);
    EXPECT_LT(icon_at, direct.find("</div>", time_at)) << "before the time text";
    EXPECT_EQ(direct.find(kRelay), std::string::npos) << direct;

    const std::string relay = RmlOf(Message("m", ours, MessageTransport::Relay), FactsWith());
    EXPECT_NE(relay.find(kRelay), std::string::npos) << relay;
    EXPECT_EQ(relay.find(kDirect), std::string::npos) << relay;
  }
}

TEST_F(TranscriptRouteIconTest, LocalOrUnknownRouteShowsNoIcon) {
  for (const auto& transport :
       {std::optional<MessageTransport>(MessageTransport::Local), std::optional<MessageTransport>()}) {
    const std::string rml = RmlOf(Message("m", true, transport), FactsWith());
    EXPECT_NE(rml.find("bubble-time"), std::string::npos) << rml;
    EXPECT_EQ(rml.find("route-"), std::string::npos) << rml;
  }
}

TEST_F(TranscriptRouteIconTest, QueuedOrFailedOutgoingShowsNoIconYet) {
  for (const SendMark mark : {SendMark::Queued, SendMark::Failed}) {
    const std::string rml = RmlOf(Message("m", true, MessageTransport::Relay), FactsWith({{"m", mark}}));
    EXPECT_NE(rml.find("send-mark"), std::string::npos) << rml;
    EXPECT_EQ(rml.find("route-"), std::string::npos) << rml;
  }
}

TEST_F(TranscriptRouteIconTest, VoiceBubbleShowsTheIconBeforeItsClock) {
  const std::string rml = RmlOf(Voice("v", false, MessageTransport::Relay), FactsWith());
  const size_t clock_at = rml.find("<span class=\"chat-voice-clock\">");
  ASSERT_NE(clock_at, std::string::npos) << rml;
  const size_t icon_at = rml.find(kRelay);
  ASSERT_NE(icon_at, std::string::npos) << rml;
  EXPECT_GT(icon_at, clock_at);
  EXPECT_LT(icon_at, rml.find("</span>", clock_at));
  const std::string none = RmlOf(Voice("v", false, MessageTransport::Local), FactsWith());
  EXPECT_EQ(none.find("route-"), std::string::npos) << none;
  const std::string queued = RmlOf(Voice("v", true, MessageTransport::Relay), FactsWith({{"v", SendMark::Queued}}));
  EXPECT_EQ(queued.find("route-"), std::string::npos) << queued;
}

} // namespace
} // namespace pbr
