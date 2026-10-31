#include "domain/attachments/AttachmentFetch.h"

#include "foundation/crypto/AttachmentContentCipher.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "common/Utilities.h"

#include <filesystem>
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

} // namespace
} // namespace pbr
