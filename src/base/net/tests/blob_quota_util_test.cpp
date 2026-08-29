#include "base/net/BlobQuotaUtil.h"
#include "base/net/BlobClient.h"
#include "base/error/AppError.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/IdentityStore.h"
#include "base/people/IdentityTypes.h"
#include "base/crypto/CryptoConstants.h"

#include <filesystem>
#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector MakeTestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(i + 1);
  }
  return dek;
}

class BlobQuotaUtilTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_blob_quota_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "test-profile");
    ASSERT_TRUE(identity_->SetDek(MakeTestDek()));
    auto created = identity_->LoadOrCreate();
    ASSERT_TRUE(static_cast<bool>(created));
    LocalIdentity registered = *created;
    registered.registered = true;
    registered.relay_user_id = "relay:quota-test";
    ASSERT_TRUE(static_cast<bool>(identity_->Update(registered)));
  }

  void TearDown() override {
    identity_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<IdentityStore> identity_;
};

RelayBlobRecord MakeBlob(const std::string& blob_id, const std::string& created_at, BlobPurpose purpose) {
  RelayBlobRecord record;
  record.blob_id = blob_id;
  record.created_at = created_at;
  record.purpose = purpose;
  record.byte_length = 1024;
  record.status = BlobStatus::Retained;
  return record;
}

TEST_F(BlobQuotaUtilTest, IsBlobQuotaErrorDetectsQuotaExceeded) {
  const Error quota = AppError::Blob(Err::Blob::QuotaExceeded, "Small file quota exceeded");
  EXPECT_TRUE(IsBlobQuotaError(quota));
  EXPECT_FALSE(IsBlobQuotaError(AppError::Network(Err::Network::HttpError, "HTTP 500")));
}

TEST_F(BlobQuotaUtilTest, PlanOldestRelayBlobDeletionSkipsProtectedBlob) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeBlob("newer", "2026-08-24T12:00:00.000Z", BlobPurpose::File),
                  MakeBlob("oldest", "2026-08-01T00:00:00.000Z", BlobPurpose::File),
                  MakeBlob("icon", "2026-01-01T00:00:00.000Z", BlobPurpose::Icon)};
  blob.SetListResult(listed);

  auto plan = PlanOldestRelayBlobDeletion(blob, *identity_, "icon");
  ASSERT_TRUE(static_cast<bool>(plan));
  EXPECT_EQ(plan->blob_to_delete.blob_id, "oldest");
}

TEST_F(BlobQuotaUtilTest, FreeOldestRelayBlobSlotDeletesOldestRemoteOnly) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeBlob("keep", "2026-08-24T12:00:00.000Z", BlobPurpose::File),
                  MakeBlob("delete-me", "2026-08-01T00:00:00.000Z", BlobPurpose::File)};
  blob.SetListResult(listed);

  auto freed = FreeOldestRelayBlobSlot(blob, *identity_);
  ASSERT_TRUE(static_cast<bool>(freed));
  ASSERT_EQ(blob.DeletedBlobIds().size(), 1u);
  EXPECT_EQ(blob.DeletedBlobIds().front(), "delete-me");
}

TEST_F(BlobQuotaUtilTest, UploadRelayBlobBytesSurfacesQuotaErrorFromPresign) {
  MockBlobClient blob;
  blob.SetPresignError(AppError::Blob(Err::Blob::QuotaExceeded, "Small file quota exceeded"));

  auto uploaded = UploadRelayBlobBytes(blob, "relay:u1", "application/octet-stream", BlobPurpose::File, "body");
  ASSERT_FALSE(static_cast<bool>(uploaded));
  EXPECT_TRUE(IsBlobQuotaError(uploaded.error()));
  EXPECT_EQ(blob.PresignCallCount(), 1u);
  EXPECT_TRUE(blob.UploadedBodies().empty());
}

} // namespace
} // namespace pbr
