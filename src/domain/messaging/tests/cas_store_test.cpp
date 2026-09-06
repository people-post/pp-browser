#include "domain/messaging/CasStore.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <gtest/gtest.h>

namespace pbr {
namespace {

class CasStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_cas_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
            std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    store_ = std::make_unique<CasStore>(dir_.string(), "profile-a");
  }

  void TearDown() override {
    store_.reset();
    std::filesystem::remove_all(dir_);
  }

  ByteVector MakeDek(const uint8_t fill) const { return ByteVector(kDataEncryptionKeySize, fill); }

  std::filesystem::path dir_;
  std::unique_ptr<CasStore> store_;
  const ByteVector plain_{'c', 'a', 's', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd'};
};

TEST_F(CasStoreTest, PrivatePutGetRoundTripAndPpbaMagic) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x42);

  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, dek, "text/plain", "note.txt")) << "put failed";
  EXPECT_TRUE(store_->Exists(CasRealm::Private, *hash));
  EXPECT_FALSE(store_->Exists(CasRealm::Public, *hash));

  auto loaded = store_->GetPrivate(*hash, dek);
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(*loaded, plain_);

  auto meta = store_->Index().Lookup(CasRealm::Private, *hash);
  ASSERT_TRUE(meta) << meta.error().message;
  ASSERT_TRUE(meta->has_value());
  EXPECT_EQ((*meta)->mime, "text/plain");
  EXPECT_EQ((*meta)->filename, "note.txt");
  EXPECT_EQ((*meta)->byte_length, plain_.size());

  std::ifstream in(store_->BlockPath(CasRealm::Private, *hash), std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(in));
  char magic[4] = {};
  in.read(magic, 4);
  EXPECT_EQ(std::string(magic, 4), "PPBA");

  const std::filesystem::path path = store_->BlockPath(CasRealm::Private, *hash);
  const std::string hex = BytesToHex(*hash);
  ASSERT_GE(hex.size(), 4u);
  // generic_string() uses '/' on all platforms (Windows CI returns '\' from native paths).
  const std::string generic = path.generic_string();
  EXPECT_NE(generic.find("/blocks/" + hex.substr(0, 2) + "/" + hex.substr(2, 2) + "/" + hex), std::string::npos);
}

TEST_F(CasStoreTest, PrivateWrongDekFails) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, MakeDek(0x11)));
  auto loaded = store_->GetPrivate(*hash, MakeDek(0x22));
  EXPECT_FALSE(static_cast<bool>(loaded));
}

TEST_F(CasStoreTest, PublicPutGetClearAndRealmIsolation) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x7);

  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, dek, "text/plain", "private.txt"));
  ASSERT_TRUE(store_->PutPublic(*hash, plain_, "text/plain", "public.txt", BytesToHex(*hash)));

  auto pub = store_->GetPublic(*hash);
  ASSERT_TRUE(pub) << pub.error().message;
  EXPECT_EQ(*pub, plain_);

  // Public file must not be PPBA-wrapped.
  std::ifstream in(store_->BlockPath(CasRealm::Public, *hash), std::ios::binary);
  char magic[4] = {};
  in.read(magic, 4);
  EXPECT_NE(std::string(magic, static_cast<size_t>(in.gcount())), "PPBA");

  auto priv = store_->GetPrivate(*hash, dek);
  ASSERT_TRUE(priv);
  EXPECT_EQ(*priv, plain_);

  auto pub_meta = store_->Index().Lookup(CasRealm::Public, *hash);
  ASSERT_TRUE(pub_meta && pub_meta->has_value());
  EXPECT_EQ((*pub_meta)->published_from_hex, BytesToHex(*hash));
}

TEST_F(CasStoreTest, ContentIdMismatchRejected) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  ByteVector wrong = *hash;
  wrong[0] ^= 0xff;
  auto put = store_->PutPrivate(wrong, plain_, MakeDek(0x1));
  EXPECT_FALSE(static_cast<bool>(put));
}

TEST_F(CasStoreTest, DeleteRemovesBlockAndIndex) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, MakeDek(0x9)));
  ASSERT_TRUE(store_->Delete(CasRealm::Private, *hash));
  EXPECT_FALSE(store_->Exists(CasRealm::Private, *hash));
  auto meta = store_->Index().Lookup(CasRealm::Private, *hash);
  ASSERT_TRUE(meta);
  EXPECT_FALSE(meta->has_value());
}


TEST_F(CasStoreTest, ListFiltersByRealmAndPinned) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x31);
  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, dek, "text/plain", "kept.txt", true));

  const ByteVector other_plain{'c', 'a', 'c', 'h', 'e', '-', 'b', 'y', 't', 'e', 's', '!'};
  auto other_hash = AttachmentContentHash(other_plain);
  ASSERT_TRUE(other_hash);
  ASSERT_TRUE(store_->PutPrivate(*other_hash, other_plain, dek, "text/plain", "cache.txt", false));

  auto all_private = store_->Index().List(CasRealm::Private, std::nullopt);
  ASSERT_TRUE(all_private) << all_private.error().message;
  EXPECT_EQ(all_private->size(), 2u);

  auto kept = store_->Index().List(CasRealm::Private, true);
  ASSERT_TRUE(kept);
  ASSERT_EQ(kept->size(), 1u);
  EXPECT_EQ((*kept)[0].filename, "kept.txt");
  EXPECT_TRUE((*kept)[0].pinned);

  auto cache = store_->Index().List(CasRealm::Private, false);
  ASSERT_TRUE(cache);
  ASSERT_EQ(cache->size(), 1u);
  EXPECT_EQ((*cache)[0].filename, "cache.txt");
  EXPECT_FALSE((*cache)[0].pinned);
}

TEST_F(CasStoreTest, PublishFromPrivatePinsPublicAndUnpublishCaches) {
  auto hash = AttachmentContentHash(plain_);
  ASSERT_TRUE(hash);
  const auto dek = MakeDek(0x55);
  ASSERT_TRUE(store_->PutPrivate(*hash, plain_, dek, "image/png", "photo.png"));

  auto published = store_->PublishFromPrivate(*hash, dek);
  ASSERT_TRUE(published) << published.error().message;
  EXPECT_EQ(*published, *hash); // v1 published bytes == plaintext
  EXPECT_TRUE(store_->Exists(CasRealm::Public, *published));

  auto pub_meta = store_->Index().Lookup(CasRealm::Public, *published);
  ASSERT_TRUE(pub_meta && pub_meta->has_value());
  EXPECT_TRUE((*pub_meta)->pinned);
  EXPECT_EQ((*pub_meta)->published_from_hex, BytesToHex(*hash));
  EXPECT_EQ((*pub_meta)->filename, "photo.png");

  auto public_kept = store_->Index().List(CasRealm::Public, true);
  ASSERT_TRUE(public_kept);
  EXPECT_EQ(public_kept->size(), 1u);

  ASSERT_TRUE(store_->Unpublish(*published));
  auto after = store_->Index().Lookup(CasRealm::Public, *published);
  ASSERT_TRUE(after && after->has_value());
  EXPECT_FALSE((*after)->pinned);
  EXPECT_TRUE(store_->Exists(CasRealm::Public, *published)); // bytes remain until wipe/GC

  auto public_cache = store_->Index().List(CasRealm::Public, false);
  ASSERT_TRUE(public_cache);
  EXPECT_EQ(public_cache->size(), 1u);
}

} // namespace
} // namespace pbr
