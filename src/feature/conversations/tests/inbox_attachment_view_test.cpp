#include "domain/peers/DirectoryShadowCache.h"
#include "domain/peers/PeerDisplaySources.h"
#include "feature/conversations/InboxController.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

// A picture received from a contact is shown inline only when its declared size is within the pixel cap;
// otherwise the card is a file row and the bytes are never decoded.
class InboxAttachmentViewTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_inbox_view_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    inbox_->SetProfileDataDir(data_dir_.string());
  }
  void TearDown() override {
    inbox_.reset();
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  /** A received picture whose view file (`blobs_view/<hash>.png`) holds `bytes`. */
  TranscriptAttachmentView ViewOf(const uint8_t seed, const std::vector<uint8_t>& bytes) {
    ChatAttachmentFields fields;
    fields.url = "https://cdn.example/pic";
    fields.mime = "image/png";
    fields.filename = "pic.png";
    fields.byte_length = bytes.size();
    fields.content_hash.assign(32, seed);
    fields.content_key.assign(kSessionKeySize, 1);
    fields.blob_nonce.assign(kAeadNonceSize, 2);
    const auto view_dir = std::filesystem::path(AttachmentViewRoot(data_dir_.string(), "t1"));
    std::filesystem::create_directories(view_dir);
    std::ofstream out(view_dir / (AttachmentHashHex(fields.content_hash) + ".png"), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    ThreadMessage message;
    message.id = "m" + std::to_string(seed);
    message.thread_id = "t1";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return inbox_->Facts().attachment(message);
  }

  static std::vector<uint8_t> PngHeader(const uint32_t w, const uint32_t h) {
    return {0x89, 'P',  'N',  'G',  0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
            static_cast<uint8_t>(w >> 24), static_cast<uint8_t>(w >> 16), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(w),
            static_cast<uint8_t>(h >> 24), static_cast<uint8_t>(h >> 16), static_cast<uint8_t>(h >> 8), static_cast<uint8_t>(h),
            8, 2, 0, 0, 0};
  }

  std::filesystem::path data_dir_;
  MockDirectoryClient directory_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<DirectoryShadowCache> shadows_;
  std::unique_ptr<PeerDisplayResolver> labels_;
  std::unique_ptr<InboxController> inbox_;
};

TEST_F(InboxAttachmentViewTest, AHeaderWithinTheCapIsShownInline) {
  const auto view = ViewOf(1, PngHeader(1000, 1000));
  EXPECT_FALSE(view.local_path.empty());
  EXPECT_TRUE(view.inline_image_ok);
}

TEST_F(InboxAttachmentViewTest, AHeaderClaiming30000By30000IsAFileRowAndNeverDecoded) {
  const auto view = ViewOf(2, PngHeader(30000, 30000));
  EXPECT_FALSE(view.local_path.empty()) << "it can still be opened by the system viewer";
  EXPECT_FALSE(view.inline_image_ok);
}

TEST_F(InboxAttachmentViewTest, AnUnreadableHeadIsAFileRowToo) {
  const auto view = ViewOf(3, {'n', 'o', 't', ' ', 'a', ' ', 'p', 'i', 'c', 't', 'u', 'r', 'e'});
  EXPECT_FALSE(view.inline_image_ok);
}

} // namespace
} // namespace pbr
