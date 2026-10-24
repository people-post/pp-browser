// A contact card comes from the peer: its avatar_url is never used as a local image path.
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "domain/peers/PeerDisplaySources.h"
#include "gui/chat/TranscriptRml.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xc0 + i);
  }
  return dek;
}

class TranscriptContactCardTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_contact_card_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    Thread thread;
    thread.id = "thread-card";
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

  // Appends a card carrying `avatar_url` and returns its rendered RML.
  std::string CardRml(const std::string& avatar_url) {
    const std::string id = "card-" + util::GenerateUuid();
    ThreadMessage message;
    message.id = id;
    message.thread_id = "thread-card";
    message.sender_contact_id = "account:peer";
    message.content_type = ChatContentType::ContactCard;
    message.text = "Alice";
    // Escape backslashes for JSON.
    std::string escaped;
    for (char c : avatar_url) {
      if (c == '\\') {
        escaped += "\\\\";
      } else {
        escaped += c;
      }
    }
    message.payload_json = R"({"contact_id":"contact:abc","display_name":"Alice","avatar_url":")" + escaped + R"("})";
    message.timestamp = util::NowUnixMs();
    EXPECT_TRUE(store_->AppendMessage(message));
    for (const MessageDisplayRow& row : RenderTranscript(inbox_->BuildTranscript("thread-card"), inbox_->Facts())) {
      if (std::string(row.message_id.c_str()) == id) {
        return row.content_rml.c_str();
      }
    }
    return "<no row>";
  }

  void ExpectNotRendered(const std::string& avatar_url) {
    const std::string rml = CardRml(avatar_url);
    EXPECT_EQ(rml.find(avatar_url), std::string::npos) << rml;
    EXPECT_EQ(rml.find("<img"), std::string::npos) << rml;
    EXPECT_NE(rml.find("Alice"), std::string::npos) << rml;
  }

  std::filesystem::path data_dir_;
  MockDirectoryClient directory_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<DirectoryShadowCache> shadows_;
  std::unique_ptr<PeerDisplayResolver> labels_;
  std::unique_ptr<InboxController> inbox_;
};

TEST_F(TranscriptContactCardTest, AbsolutePathAvatarIsNotRendered) {
  ExpectNotRendered("/etc/hosts");
}

TEST_F(TranscriptContactCardTest, ParentDirectoryAvatarIsNotRendered) {
  ExpectNotRendered("../../../etc/hosts");
}

TEST_F(TranscriptContactCardTest, UncAvatarIsNotRendered) {
  ExpectNotRendered("\\\\attacker\\share\\a.png");
}

TEST_F(TranscriptContactCardTest, FileUrlAvatarIsNotRendered) {
  ExpectNotRendered("file:///etc/hosts");
}

TEST_F(TranscriptContactCardTest, NonRegularFileAvatarIsNotRendered) {
  ExpectNotRendered("/dev/zero");
}

} // namespace
} // namespace pbr
