#include "domain/attachments/AttachmentFetch.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "common/Utilities.h"
#include "common/tests/LocalHttpServer.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace pbr {
namespace {

// The sender's byte_length is a claim. Whatever the ciphertext's way here (a pending push, a peer, the CDN), it
// must be exactly that many bytes plus the AEAD tag before anything is decrypted.
class FetchSizeTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_fetch_size_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    plain_.assign(1000, 0x5a);
    auto prepared = PrepareChatAttachmentFromBytes(plain_, "doc.pdf", "", 1'000'000);
    ASSERT_TRUE(prepared) << prepared.error().message;
    fields_ = prepared->fields;
    cipher_ = prepared->ciphertext;
    context_.thread_id = "t1";
    context_.profile_data_dir = dir_.string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  Roe<std::vector<uint8_t>> RunWith(const std::vector<uint8_t>& cipher, ChatAttachmentFields fields) {
    EXPECT_TRUE(SavePendingAttachmentCiphertext(dir_.string(), "t1", fields.content_hash, cipher, {}));
    return FetchAndDecryptAttachment(fields, context_);
  }
  bool PendingLeft() const { return AttachmentPendingCiphertextExists(dir_.string(), "t1", fields_.content_hash); }

  std::filesystem::path dir_;
  std::vector<uint8_t> plain_;
  ChatAttachmentFields fields_;
  std::vector<uint8_t> cipher_;
  AttachmentFetchContext context_;
};

TEST_F(FetchSizeTest, TheOverheadConstantIsTheRealOverhead) {
  EXPECT_EQ(cipher_.size(), plain_.size() + kAttachmentCipherOverheadBytes);
}

TEST_F(FetchSizeTest, ExactlyTheDeclaredSizeDecrypts) {
  auto result = RunWith(cipher_, fields_);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, plain_);
}

TEST_F(FetchSizeTest, ADifferentCiphertextLengthIsRefusedAndDropped) {
  auto longer = cipher_;
  longer.push_back(0);
  auto too_long = RunWith(longer, fields_);
  ASSERT_FALSE(too_long);
  EXPECT_NE(too_long.error().message.find("size"), std::string::npos);
  EXPECT_FALSE(PendingLeft()) << "the bad copy is dropped so a retry re-fetches";

  auto shorter = cipher_;
  shorter.pop_back();
  EXPECT_FALSE(RunWith(shorter, fields_));
  EXPECT_FALSE(PendingLeft());
}

TEST_F(FetchSizeTest, ALyingDeclaredSizeIsRefused) {
  ChatAttachmentFields small_claim = fields_;
  small_claim.byte_length = 10;  // claims 10 bytes, the object is 1000
  EXPECT_FALSE(RunWith(cipher_, small_claim));
  ChatAttachmentFields big_claim = fields_;
  big_claim.byte_length = 5'000'000;
  EXPECT_FALSE(RunWith(cipher_, big_claim));
}

TEST_F(FetchSizeTest, ZeroDeclaredSizeIsNeverFetched) {
  ChatAttachmentFields zero = fields_;
  zero.byte_length = 0;
  EXPECT_FALSE(RunWith(cipher_, zero));
  zero.url = "https://cdn.example/x";  // the CDN path also refuses before any request
  auto cdn = FetchAttachmentCiphertext(zero, AttachmentFetchContext{});
  ASSERT_FALSE(cdn);
  EXPECT_NE(cdn.error().message.find("no size"), std::string::npos);
}

// The CDN path: streamed to a temp file under the profile, capped at the declared size, no buffered-client limit.
class CdnFetchTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_cdn_fetch_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    context_.thread_id = "t1";
    context_.profile_data_dir = dir_.string();
    context_.allow_local_cdn_for_test = true;
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  PreparedChatAttachment Prepare(const size_t bytes) {
    plain_.assign(bytes, 0x37);
    auto prepared = PrepareChatAttachmentFromBytes(plain_, "clip.mp4", "", 200'000'000);
    EXPECT_TRUE(prepared);
    return *prepared;
  }
  bool NoLeftover() const {
    std::error_code ec;
    const auto incoming = std::filesystem::path(IncomingCiphertextDir(dir_.string()));
    return !std::filesystem::exists(incoming) || std::filesystem::is_empty(incoming, ec);
  }
  std::filesystem::path dir_;
  std::vector<uint8_t> plain_;
  AttachmentFetchContext context_;
  pbr::test::LocalHttpServer server_;
};

TEST_F(CdnFetchTest, ADownloadAboveTheBufferedClientsLimitIsStreamedAndDecrypted) {
  auto prepared = Prepare(9 * 1024 * 1024);  // the buffered client stops at 8 MiB
  server_.SetResponse(std::string(prepared.ciphertext.begin(), prepared.ciphertext.end()));
  prepared.fields.url = server_.Url();
  uint64_t last_done = 0;
  uint64_t last_total = 0;
  context_.progress = [&](const uint64_t done, const uint64_t total) {
    last_done = done;
    last_total = total;
    return true;
  };
  auto result = FetchAndDecryptAttachment(prepared.fields, context_);
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(*result, plain_);
  EXPECT_EQ(last_done, prepared.ciphertext.size());
  EXPECT_EQ(last_total, prepared.ciphertext.size());
  EXPECT_TRUE(NoLeftover());
}

TEST_F(CdnFetchTest, AnObjectLongerThanDeclaredIsAbortedAndNothingIsLeftBehind) {
  auto prepared = Prepare(100'000);
  server_.SetResponse(std::string(prepared.ciphertext.begin(), prepared.ciphertext.end()) + std::string(5000, 'x'));
  prepared.fields.url = server_.Url();
  auto result = FetchAndDecryptAttachment(prepared.fields, context_);
  ASSERT_FALSE(result);
  EXPECT_TRUE(NoLeftover());
}

TEST_F(CdnFetchTest, AnObjectShorterThanDeclaredIsRefused) {
  auto prepared = Prepare(100'000);
  server_.SetResponse(std::string(prepared.ciphertext.begin(), prepared.ciphertext.end() - 100));
  prepared.fields.url = server_.Url();
  EXPECT_FALSE(FetchAndDecryptAttachment(prepared.fields, context_));
  EXPECT_TRUE(NoLeftover());
}

TEST_F(CdnFetchTest, InterruptedDownloadsAreCleanedAtStart) {
  const auto dir = std::filesystem::path(IncomingCiphertextDir(dir_.string()));
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "left.part") << "partial";
  CleanIncomingCiphertext(dir_.string());
  EXPECT_FALSE(std::filesystem::exists(dir / "left.part"));
}

} // namespace
} // namespace pbr
