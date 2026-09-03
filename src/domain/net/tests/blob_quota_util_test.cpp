#include "domain/net/BlobQuotaUtil.h"
#include "domain/net/BlobClient.h"
#include "foundation/error/AppError.h"
#include "domain/net/ServiceClientsImpl.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

RelayBlobRecord MakeBlob(const std::string& blob_id, const std::string& created_at, BlobPurpose purpose) {
  RelayBlobRecord record;
  record.blob_id = blob_id;
  record.created_at = created_at;
  record.purpose = purpose;
  record.byte_length = 1024;
  record.status = BlobStatus::Retained;
  return record;
}

TEST(BlobQuotaUtilTest, IsBlobQuotaErrorDetectsQuotaExceeded) {
  const Error quota = AppError::Blob(Err::Blob::QuotaExceeded, "Small file quota exceeded");
  EXPECT_TRUE(IsBlobQuotaError(quota));
  EXPECT_FALSE(IsBlobQuotaError(AppError::Network(Err::Network::HttpError, "HTTP 500")));
}

TEST(BlobQuotaUtilTest, PlanOldestRelayBlobDeletionSkipsProtectedBlob) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeBlob("newer", "2026-08-24T12:00:00.000Z", BlobPurpose::File),
                  MakeBlob("oldest", "2026-08-01T00:00:00.000Z", BlobPurpose::File),
                  MakeBlob("icon", "2026-01-01T00:00:00.000Z", BlobPurpose::Icon)};
  blob.SetListResult(listed);

  auto plan = PlanOldestRelayBlobDeletion(blob, "relay:quota-test", "icon");
  ASSERT_TRUE(static_cast<bool>(plan));
  EXPECT_EQ(plan->blob_to_delete.blob_id, "oldest");
}

TEST(BlobQuotaUtilTest, FreeOldestRelayBlobSlotDeletesOldestRemoteOnly) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeBlob("keep", "2026-08-24T12:00:00.000Z", BlobPurpose::File),
                  MakeBlob("delete-me", "2026-08-01T00:00:00.000Z", BlobPurpose::File)};
  blob.SetListResult(listed);

  auto freed = FreeOldestRelayBlobSlot(blob, "relay:quota-test");
  ASSERT_TRUE(static_cast<bool>(freed));
  ASSERT_EQ(blob.DeletedBlobIds().size(), 1u);
  EXPECT_EQ(blob.DeletedBlobIds().front(), "delete-me");
}

TEST(BlobQuotaUtilTest, EmptyRelayUserIdRejected) {
  MockBlobClient blob;
  auto plan = PlanOldestRelayBlobDeletion(blob, "");
  ASSERT_FALSE(static_cast<bool>(plan));
  EXPECT_NE(plan.error().message.find("Register"), std::string::npos);
}

TEST(BlobQuotaUtilTest, UploadRelayBlobBytesSurfacesQuotaErrorFromPresign) {
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
