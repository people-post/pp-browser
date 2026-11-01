#include "domain/attachments/OutgoingAttachmentStore.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "common/Utilities.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector Dek(const uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

class OutgoingStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_outgoing_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    store_ = std::make_unique<OutgoingAttachmentStore>(dir_.string(), "profile-1");
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  OutgoingAttachmentRecord Record(const std::string& id, const std::string& thread, const int64_t created_ms = 1) {
    OutgoingAttachmentRecord record;
    record.id = id;
    record.thread_id = thread;
    record.created_ms = created_ms;
    record.fields.mime = "video/mp4";
    record.fields.filename = "clip.mp4";
    record.fields.byte_length = 1000;
    record.fields.content_hash.assign(kAttachmentContentHashSize, 0x11);
    record.fields.content_key.assign(kSessionKeySize, 0x7e);
    record.fields.blob_nonce.assign(kAeadNonceSize, 0x33);
    return record;
  }

  static std::string ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }

  std::filesystem::path dir_;
  std::unique_ptr<OutgoingAttachmentStore> store_;
};

TEST_F(OutgoingStoreTest, LivesUnderBlobCipherOutgoingAndRoundTripsTheKey) {
  const std::vector<uint8_t> cipher(2048, 0x5c);
  ASSERT_TRUE(store_->Begin(Record("id-1", "t1"), cipher, Dek(1)));
  EXPECT_EQ(store_->Dir(), (dir_ / "blob_cipher" / "outgoing").string());
  EXPECT_TRUE(std::filesystem::exists(dir_ / "blob_cipher" / "outgoing" / "id-1"));

  auto loaded = store_->Load("id-1", Dek(1));
  ASSERT_TRUE(loaded) << loaded.error().message;
  EXPECT_EQ(loaded->thread_id, "t1");
  EXPECT_EQ(loaded->cipher_bytes, cipher.size());
  EXPECT_EQ(loaded->fields.content_key, std::vector<uint8_t>(kSessionKeySize, 0x7e));
  EXPECT_EQ(loaded->fields.filename, "clip.mp4");
  EXPECT_FALSE(loaded->delivered);
  auto read = store_->ReadCipher("id-1");
  ASSERT_TRUE(read);
  EXPECT_EQ(*read, cipher);
}

TEST_F(OutgoingStoreTest, TheKeyIsOnlyInTheSealedRecordNeverInTheClearOrNextToTheCiphertext) {
  ASSERT_TRUE(store_->Begin(Record("id-2", "t1"), std::vector<uint8_t>(64, 1), Dek(1)));
  const std::string rec = ReadAll(store_->CipherPath("id-2") + ".rec");
  EXPECT_EQ(rec.find("content_key"), std::string::npos) << "sealed, not readable JSON";
  EXPECT_EQ(rec.find("clip.mp4"), std::string::npos);
  EXPECT_FALSE(store_->Load("id-2", Dek(2))) << "another key cannot open it";
}

TEST_F(OutgoingStoreTest, UpdateKeepsTheSameCiphertextAndMarksDelivery) {
  const std::vector<uint8_t> cipher(100, 9);
  auto record = Record("id-3", "t1");
  ASSERT_TRUE(store_->Begin(record, cipher, Dek(1)));
  record.delivered = true;
  record.fields.url = "https://cdn.example/x";
  ASSERT_TRUE(store_->Update(record, Dek(1)));
  auto loaded = store_->Load("id-3", Dek(1));
  ASSERT_TRUE(loaded);
  EXPECT_TRUE(loaded->delivered);
  EXPECT_EQ(loaded->fields.url, "https://cdn.example/x");
  EXPECT_EQ(*store_->ReadCipher("id-3"), cipher);
}

TEST_F(OutgoingStoreTest, ListsOnlyThisThreadsWhoseCiphertextIsStillThereOldestFirst) {
  ASSERT_TRUE(store_->Begin(Record("b", "t1", 20), {1, 2, 3}, Dek(1)));
  ASSERT_TRUE(store_->Begin(Record("a", "t1", 10), {1, 2, 3}, Dek(1)));
  ASSERT_TRUE(store_->Begin(Record("c", "t2", 5), {1, 2, 3}, Dek(1)));
  ASSERT_TRUE(store_->Begin(Record("gone", "t1", 1), {1, 2, 3}, Dek(1)));
  std::filesystem::remove(store_->CipherPath("gone"));  // ciphertext lost: nothing to resume from

  const auto listed = store_->ListForThread("t1", Dek(1));
  ASSERT_EQ(listed.size(), 2u);
  EXPECT_EQ(listed[0].id, "a");
  EXPECT_EQ(listed[1].id, "b");
  EXPECT_TRUE(store_->ListForThread("t1", Dek(2)).empty()) << "locked / wrong key: none";
}

// A resume deletes a record only when it is confirmed damaged: the load says which of the two it is.
TEST_F(OutgoingStoreTest, ALoadTellsAGoneRecordFromADamagedOne) {
  auto gone = store_->Load("never-existed", Dek(1));
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().code, OutgoingAttachmentStore::kLoadGone);

  ASSERT_TRUE(store_->Begin(Record("dmg", "t1"), {1, 2, 3}, Dek(1)));
  std::ofstream(store_->CipherPath("dmg") + ".rec", std::ios::binary | std::ios::trunc) << "not a sealed record";
  auto damaged = store_->Load("dmg", Dek(1));
  ASSERT_FALSE(damaged);
  EXPECT_EQ(damaged.error().code, OutgoingAttachmentStore::kLoadDamaged);
  EXPECT_TRUE(store_->HasCipher("dmg")) << "Load itself deletes nothing";
}

TEST_F(OutgoingStoreTest, RemoveDeletesBothFilesAndRefusesUnsafeIds) {
  ASSERT_TRUE(store_->Begin(Record("id-4", "t1"), {1}, Dek(1)));
  store_->Remove("id-4");
  EXPECT_FALSE(store_->HasCipher("id-4"));
  EXPECT_FALSE(std::filesystem::exists(store_->CipherPath("id-4") + ".rec"));
  EXPECT_FALSE(store_->Begin(Record("../escape", "t1"), {1}, Dek(1)));
}

TEST_F(OutgoingStoreTest, AfterADayEverythingStaleGoesAndFreshStays) {
  ASSERT_TRUE(store_->Begin(Record("old", "t1"), {1, 2}, Dek(1)));
  ASSERT_TRUE(store_->Begin(Record("fresh", "t1"), {1, 2}, Dek(1)));
  std::ofstream(store_->Dir() + "/stray") << "orphan ciphertext without a record";
  const auto two_days_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
  for (const char* name : {"old", "old.rec", "stray"}) {
    std::filesystem::last_write_time(std::filesystem::path(store_->Dir()) / name, two_days_ago);
  }
  EXPECT_EQ(store_->CleanupStale(OutgoingAttachmentStore::kMaxAgeMs), 2u);
  EXPECT_FALSE(store_->HasCipher("old"));
  EXPECT_FALSE(std::filesystem::exists(store_->Dir() + "/stray"));
  EXPECT_TRUE(store_->HasCipher("fresh"));
}

} // namespace
} // namespace pbr
