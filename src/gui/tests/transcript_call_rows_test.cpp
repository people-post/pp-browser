// A call summary is drawn as a pill with an icon on the starter's side, not as a centered system line.
#include "domain/calls/CallControlCodec.h"
#include "domain/calls/CallTypes.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/net/testing/MockOrgBackendClients.h"
#include "domain/people/ContactsStore.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "gui/chat/TranscriptRml.h"
#include "common/Utilities.h"
#include "common/chat/SystemControl.h"

#include <filesystem>
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
  const MessageDisplayRow row =
      Row(kLocalSelfContactId, CallMediaMode::Voice, CallOutcome::Missed, std::nullopt, /*group=*/true, /*started=*/false);
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

} // namespace
} // namespace pbr
