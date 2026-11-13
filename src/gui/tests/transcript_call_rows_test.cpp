// A call summary is drawn as a pill with an icon on the starter's side, not as a centered system line.
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallControlFamily.h"
#include "lib/calls/CallTypes.h"
#include "lib/storage/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "gui/chat/TranscriptRml.h"
#include "common/Utilities.h"
#include "common/chat/SystemControl.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xd0 + i);
  }
  return dek;
}

class TranscriptCallRowsTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_rows_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    // The calls family as the app registers it: owns call control messages, no line of its own here.
    SystemControlFamily call;
    call.name = "call";
    call.owns = [](const ThreadMessage& m) { return m.payload_json.find("call_") != std::string::npos; };
    call.on_lane = [](const ThreadMessage& m) { return m.payload_json.find("call_summary") == std::string::npos; };
    controls_.Bind(call);
    inbox_->SetSystemControls(&controls_);
    Thread thread;
    thread.id = "thread-calls";
    thread.kind = ThreadKind::Direct;
    thread.title = "Peer";
    thread.updated_at = 1;
    ASSERT_TRUE(store_->UpsertThread(thread));
  }

  void TearDown() override {
    inbox_.reset();
    labels_.reset();
    shadows_.reset();
    contacts_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  // Appends a call summary written by `sender` and returns that row as rendered.
  MessageDisplayRow Row(const std::string& sender, CallMediaMode mode, CallOutcome outcome,
                        std::optional<int64_t> duration_ms, bool group = false, bool started = false) {
    CallSummaryDetail detail;
    detail.call_id = "call:" + util::GenerateUuid();
    detail.media_mode = mode;
    detail.outcome = outcome;
    detail.duration_ms = duration_ms;
    detail.group = group;
    detail.started = started;
    auto encoded = CallControlCodec::EncodeSummary(detail);
    EXPECT_TRUE(encoded);
    auto built = CallControlCodec::BuildSystemMessage("thread-calls", CallControlType::CallSummary, "call",
                                                      encoded ? *encoded : std::string("{}"), sender);
    EXPECT_TRUE(built);
    ThreadMessage message = built ? *built : ThreadMessage{};
    message.id = "row-" + util::GenerateUuid();
    message.timestamp = util::NowUnixMs();
    EXPECT_TRUE(store_->AppendMessage(message));
    for (const MessageDisplayRow& row : RenderTranscript(inbox_->BuildTranscript("thread-calls"), inbox_->Facts())) {
      if (std::string(row.message_id.c_str()) == message.id) {
        return row;
      }
    }
    return MessageDisplayRow{};
  }

  std::filesystem::path data_dir_;
  MockDirectoryClient directory_;
  SystemControlRegistry controls_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<DirectoryShadowCache> shadows_;
  std::unique_ptr<PeerDisplayResolver> labels_;
  std::unique_ptr<InboxController> inbox_;
};

TEST_F(TranscriptCallRowsTest, OurVoiceCallIsAPillOnOurSideWithThePhoneIcon) {
  const MessageDisplayRow row = Row(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Completed, 71'000);
  const std::string rml = row.content_rml.c_str();
  EXPECT_EQ(std::string(row.row_class.c_str()), "message-row-user");
  EXPECT_NE(rml.find("chat-call-row"), std::string::npos) << rml;
  EXPECT_NE(rml.find("../icons/phone.svg"), std::string::npos) << rml;
  EXPECT_NE(rml.find("1:11"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("chat-system-line"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("muted"), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, TheirVideoCallSitsOnTheirSideWithTheCameraIcon) {
  const MessageDisplayRow row = Row("account:peer", CallMediaMode::Video, CallOutcome::Completed, 56'000);
  const std::string rml = row.content_rml.c_str();
  EXPECT_EQ(std::string(row.row_class.c_str()), "message-row-peer");
  EXPECT_NE(rml.find("../icons/video.svg"), std::string::npos) << rml;
  EXPECT_NE(rml.find("0:56"), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, AGroupCallSomeoneElseStartedSitsOnTheirSide) {
  // A group summary is written by this device (sender = self); the side is who started the call.
  const MessageDisplayRow row = Row(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Missed, std::nullopt,
                                    /*group=*/true, /*started=*/false);
  EXPECT_EQ(std::string(row.row_class.c_str()), "message-row-peer");
  const MessageDisplayRow ours =
      Row(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Completed, 30'000, /*group=*/true, /*started=*/true);
  EXPECT_EQ(std::string(ours.row_class.c_str()), "message-row-user");
}

TEST_F(TranscriptCallRowsTest, AMissedCallIsMutedAndHasNoDuration) {
  const MessageDisplayRow row = Row("account:peer", CallMediaMode::Voice, CallOutcome::Missed, std::nullopt);
  const std::string rml = row.content_rml.c_str();
  EXPECT_NE(rml.find("chat-call-row muted"), std::string::npos) << rml;
  EXPECT_EQ(rml.find(':'), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, ADirectVoiceRecordCallsBackWithVoice) {
  const std::string rml =
      std::string(Row("account:peer", CallMediaMode::Voice, CallOutcome::Missed, std::nullopt).content_rml.c_str());
  EXPECT_NE(rml.find("call_back('0')"), std::string::npos) << rml;
  EXPECT_NE(rml.find("chat-call-row--tappable"), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, ADirectVideoRecordCallsBackWithVideo) {
  const std::string rml =
      std::string(Row(kLocalSelfContactId, CallMediaMode::Video, CallOutcome::Completed, 9'000).content_rml.c_str());
  EXPECT_NE(rml.find("call_back('1')"), std::string::npos) << rml;
  EXPECT_NE(rml.find("chat-call-row--tappable"), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, AGroupRecordIsInert) {
  const std::string rml =
      std::string(Row(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Missed, std::nullopt, /*group=*/true)
                      .content_rml.c_str());
  EXPECT_NE(rml.find("chat-call-row"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("call_back"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("--tappable"), std::string::npos) << rml;
}

TEST_F(TranscriptCallRowsTest, ARecordInAGroupThreadIsInertEvenIfTheRowIsNotMarkedGroup) {
  CallSummaryDetail detail;
  detail.call_id = "call:x";
  detail.media_mode = CallMediaMode::Voice;
  detail.outcome = CallOutcome::Missed;
  auto encoded = CallControlCodec::EncodeSummary(detail);
  ASSERT_TRUE(encoded);
  auto built = CallControlCodec::BuildSystemMessage("thread-calls", CallControlType::CallSummary, "call", *encoded,
                                                    "account:peer");
  ASSERT_TRUE(built);
  ThreadMessage message = *built;
  message.id = "row-in-group";
  message.timestamp = util::NowUnixMs();
  ASSERT_TRUE(store_->AppendMessage(message));
  TranscriptFacts facts = inbox_->Facts();
  facts.thread_is_group = true;
  bool found = false;
  for (const MessageDisplayRow& row : RenderTranscript(inbox_->BuildTranscript("thread-calls"), facts)) {
    if (std::string(row.message_id.c_str()) == message.id) {
      found = true;
      EXPECT_EQ(std::string(row.content_rml.c_str()).find("call_back"), std::string::npos);
    }
  }
  EXPECT_TRUE(found);
}

// Kenneth 2026-11-12: a call record row opens no message menu (one tap calls back). ChatController::IsCallRecord asks
// CallSummaryRowOf, ChatMenus::ForRequest returns an empty menu for it; ForRequest needs a live facade, so its order is
// pinned by source like the other menu rules.
ThreadMessage CallRecordMessage(const CallMediaMode mode, const CallOutcome outcome,
                                const std::optional<int64_t> duration_ms) {
  CallSummaryDetail detail;
  detail.call_id = "call:record";
  detail.media_mode = mode;
  detail.outcome = outcome;
  detail.duration_ms = duration_ms;
  auto encoded = CallControlCodec::EncodeSummary(detail);
  EXPECT_TRUE(encoded);
  auto built = CallControlCodec::BuildSystemMessage("thread-calls", CallControlType::CallSummary, "call",
                                                    encoded ? *encoded : std::string("{}"), "account:peer");
  EXPECT_TRUE(built);
  return built ? *built : ThreadMessage{};
}

TEST(CallRecordMenuTest, VoiceVideoAndMissedRecordsAreCallRecordsAndTextIsNot) {
  EXPECT_TRUE(CallSummaryRowOf(CallRecordMessage(CallMediaMode::Voice, CallOutcome::Completed, 71'000)));
  EXPECT_TRUE(CallSummaryRowOf(CallRecordMessage(CallMediaMode::Video, CallOutcome::Completed, 56'000)));
  EXPECT_TRUE(CallSummaryRowOf(CallRecordMessage(CallMediaMode::Voice, CallOutcome::Missed, std::nullopt)));
  ThreadMessage text;
  text.content_type = ChatContentType::Text;
  text.text = "hello";
  EXPECT_FALSE(CallSummaryRowOf(text));
  ThreadMessage card;
  card.content_type = ChatContentType::ContactCard;
  EXPECT_FALSE(CallSummaryRowOf(card));
}

TEST(CallRecordMenuTest, TheMessageMenuIsEmptyForACallRecordBeforeAnythingIsOffered) {
  const auto read = [](const std::string& path) {
    std::ifstream in(std::string(PP_SOURCE_DIR) + path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string source = text.str();
    std::erase(source, '\r');
    return source;
  };
  const std::string menus = read("/src/gui/chat/ChatMenus.cpp");
  const size_t start = menus.find("std::vector<ContextMenuAction> ChatMenus::ForRequest(");
  ASSERT_NE(start, std::string::npos);
  const std::string body = menus.substr(start, menus.find("\n}\n", start) - start);
  const size_t guard = body.find("if (host_.IsCallRecord(message_id)) {\n    return actions;");
  ASSERT_NE(guard, std::string::npos);
  EXPECT_LT(guard, body.find("QuickReactions(")) << "before the reactions";
  EXPECT_LT(guard, body.find("\"reply_message\""));
  EXPECT_LT(guard, body.find("\"delete_message\""));
  const std::string controller = read("/src/gui/chat/ChatController.cpp");
  const size_t is_record = controller.find("bool ChatController::IsCallRecord(");
  ASSERT_NE(is_record, std::string::npos);
  EXPECT_NE(controller.find("CallSummaryRowOf(", is_record), std::string::npos);
}

} // namespace
} // namespace pbr
