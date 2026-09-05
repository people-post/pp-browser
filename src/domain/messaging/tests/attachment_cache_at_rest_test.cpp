#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "domain/messaging/AttachmentCache.h"
#include "domain/messaging/AttachmentPlaintextMemoryCache.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace pbr {
namespace {

class AttachmentCacheAtRestTest : public ::testing::Test {
protected:
  void SetUp() override {
    AttachmentPlaintextMemoryCache::Instance().Clear();
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_att_at_rest_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    profile_dir_ = dir_.string();
  }

  void TearDown() override {
    AttachmentPlaintextMemoryCache::Instance().Clear();
    std::filesystem::remove_all(dir_);
  }

  ByteVector MakeDek(uint8_t fill) const { return ByteVector(kDataEncryptionKeySize, fill); }

  std::filesystem::path dir_;
  std::string profile_dir_;
  const std::string thread_id_ = "thread-a";
  const std::string profile_id_ = "profile-a";
  const ByteVector plain_{'h', 'e', 'l', 'l', 'o', '-', 'b', 'l', 'o', 'b'};
};

TEST_F(AttachmentCacheAtRestTest, SaveWithDekWritesPpbaMagicAndLoads) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x42);

  auto saved = SaveAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", plain_, "note.txt", dek,
                                       profile_id_);
  ASSERT_TRUE(saved) << saved.error().message;

  std::ifstream in(*saved, std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(in));
  char magic[4] = {};
  in.read(magic, 4);
  ASSERT_EQ(in.gcount(), 4);
  EXPECT_EQ(std::string(magic, 4), "PPBA");
  // generic_string() uses '/' on all platforms (Windows CI returns '\' from native paths).
  const std::string generic = std::filesystem::path(*saved).generic_string();
  EXPECT_NE(generic.find("/cas/private/blocks/"), std::string::npos);
  EXPECT_EQ(generic.find("/blobs/"), std::string::npos);
  EXPECT_TRUE(AttachmentBlobExists(profile_dir_, thread_id_, *hash));

  auto loaded = LoadAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", dek, profile_id_);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(*loaded, plain_);
}

TEST_F(AttachmentCacheAtRestTest, WrongDekFailsDecrypt) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x11);
  const auto wrong = MakeDek(0x22);

  ASSERT_TRUE(SaveAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", plain_, "note.txt", dek,
                                      profile_id_));

  // RAM cache is DEK-session scoped (wiped on ClearDek); drop it so this checks CAS decrypt.
  AttachmentPlaintextMemoryCache::Instance().Clear();
  auto loaded = LoadAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", wrong, profile_id_);
  EXPECT_FALSE(static_cast<bool>(loaded));
}

TEST_F(AttachmentCacheAtRestTest, EnsureAttachmentViewPathMaterializesBlobsView) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x33);

  ASSERT_TRUE(SaveAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", plain_, "note.txt", dek,
                                      profile_id_));
  EXPECT_TRUE(AttachmentLocalPath(profile_dir_, thread_id_, *hash, "text/plain", "note.txt").empty());

  auto view = EnsureAttachmentViewPath(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", dek, profile_id_);
  ASSERT_TRUE(view) << view.error().message;
  EXPECT_NE(view->find("blobs_view"), std::string::npos);
  ASSERT_TRUE(std::filesystem::exists(*view));

  std::ifstream in(*view, std::ios::binary);
  const ByteVector roundtrip((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(roundtrip, plain_);

  EXPECT_EQ(AttachmentLocalPath(profile_dir_, thread_id_, *hash, "text/plain", "note.txt"), *view);
}

TEST_F(AttachmentCacheAtRestTest, AttachmentPosterPathUsesBlobsViewHash) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const std::string path = AttachmentPosterPath(profile_dir_, thread_id_, *hash);
  EXPECT_NE(path.find("blobs_view"), std::string::npos);
  EXPECT_NE(path.find(AttachmentHashHex(*hash) + ".poster.jpg"), std::string::npos);
  EXPECT_FALSE(AttachmentPosterExists(profile_dir_, thread_id_, *hash));
}

TEST_F(AttachmentCacheAtRestTest, MemoryCacheServesAfterCasRemovedAndClearsOnViewWipe) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x55);

  auto saved = SaveAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", plain_, "note.txt", dek,
                                       profile_id_);
  ASSERT_TRUE(saved) << saved.error().message;
  EXPECT_GE(AttachmentPlaintextMemoryCache::Instance().EntryCountForTest(), 1u);

  auto loaded = LoadAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", dek, profile_id_);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(*loaded, plain_);

  std::error_code ec;
  std::filesystem::remove(*saved, ec);
  ASSERT_FALSE(ec) << ec.message();
  EXPECT_FALSE(AttachmentBlobExists(profile_dir_, thread_id_, *hash));

  auto from_ram = LoadAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", dek, profile_id_);
  ASSERT_TRUE(from_ram) << from_ram.error().message;
  EXPECT_EQ(*from_ram, plain_);

  ASSERT_TRUE(WipeAllAttachmentViewCaches(profile_dir_));
  EXPECT_EQ(AttachmentPlaintextMemoryCache::Instance().EntryCountForTest(), 0u);

  auto after_wipe =
      LoadAttachmentPlaintext(profile_dir_, thread_id_, *hash, "text/plain", "note.txt", dek, profile_id_);
  EXPECT_FALSE(static_cast<bool>(after_wipe));
}

TEST_F(AttachmentCacheAtRestTest, InlinePrivateViewGateSkipsLargeVideo) {
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("image/png", 50ull * 1024ull * 1024ull));
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("video/mp4", 0));
  EXPECT_TRUE(AttachmentAllowsInlinePrivateView("video/mp4", kMaxInlinePrivateVideoBytes));
  EXPECT_FALSE(AttachmentAllowsInlinePrivateView("video/mp4", kMaxInlinePrivateVideoBytes + 1));
}

TEST_F(AttachmentCacheAtRestTest, LargeVideoPosterUsesSoftPlaceholderWithoutViewFile) {
  ByteVector big(static_cast<size_t>(kMaxInlinePrivateVideoBytes) + 16, 0xab);
  auto hash = AttachmentContentHash(big);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x66);

  ASSERT_TRUE(SaveAttachmentPlaintext(profile_dir_, thread_id_, *hash, "video/mp4", big, "clip.mp4", dek, profile_id_));

  auto poster = EnsureAttachmentPoster(profile_dir_, thread_id_, *hash, "video/mp4", "clip.mp4", dek, profile_id_,
                                       big.size());
  ASSERT_TRUE(poster) << poster.error().message;
  EXPECT_TRUE(AttachmentPosterExists(profile_dir_, thread_id_, *hash));
  EXPECT_TRUE(AttachmentLocalPath(profile_dir_, thread_id_, *hash, "video/mp4", "clip.mp4").empty());
}

} // namespace
} // namespace pbr
