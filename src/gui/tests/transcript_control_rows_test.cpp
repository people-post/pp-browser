// How a feature's control messages draw in a transcript: lines from the family, lane signaling never.
#include "common/chat/SystemControl.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

ThreadMessage Control(const std::string& id, const std::string& control_type, const std::string& text) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = "thread-rows";
  message.sender_contact_id = "account:peer";
  message.content_type = ChatContentType::System;
  message.text = text;
  message.payload_json = std::string(R"({"control_type":")") + control_type + R"(","detail":"{}"})";
  message.timestamp = util::NowUnixMs();
  return message;
}

class InboxControlRowsTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_inbox_rows_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    inbox_->SetSystemControls(&controls_);
    Thread thread;
    thread.id = "thread-rows";
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

  std::string RowRml(const std::string& message_id) {
    for (const MessageDisplayRow& row : RenderTranscript(inbox_->BuildTranscript("thread-rows"), inbox_->Facts())) {
      if (std::string(row.message_id.c_str()) == message_id) {
        return row.content_rml.c_str();
      }
    }
    return "<no row>";
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

TEST_F(InboxControlRowsTest, AFamilyWithoutALineShowsTheMessageText) {
  SystemControlFamily billing;
  billing.name = "billing";
  billing.owns = [](const ThreadMessage& m) { return m.payload_json.find("charge_required") != std::string::npos; };
  controls_.Bind(billing);
  ASSERT_TRUE(store_->AppendMessage(Control("bill-1", "charge_required", "Charge required")));
  EXPECT_NE(RowRml("bill-1").find("Charge required"), std::string::npos) << RowRml("bill-1");
}

TEST_F(InboxControlRowsTest, TheFamilysLineWinsAndLaneSignalingIsNeverARow) {
  SystemControlFamily call;
  call.name = "call";
  call.owns = [](const ThreadMessage& m) { return m.payload_json.find("call_") != std::string::npos; };
  call.on_lane = [](const ThreadMessage& m) { return m.payload_json.find("call_summary") == std::string::npos; };
  call.line = [](const ThreadMessage&, const SystemControlLabels&) { return std::optional<std::string>("Summary line"); };
  controls_.Bind(call);
  ASSERT_TRUE(store_->AppendMessage(Control("sum-1", "call_summary", "Voice call")));
  ASSERT_TRUE(store_->AppendMessage(Control("inv-1", "call_invite", "Call invite")));  // an old store's row
  EXPECT_NE(RowRml("sum-1").find("Summary line"), std::string::npos) << RowRml("sum-1");
  EXPECT_EQ(RowRml("inv-1"), "<no row>");
}

} // namespace
} // namespace pbr
