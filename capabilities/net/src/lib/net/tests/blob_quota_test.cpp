#include "lib/net/BlobQuota.h"
#include "lib/net/BlobClient.h"
#include "foundation/error/AppError.h"
#include "lib/net/testing/MockOrgBackendClients.h"

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

TEST(BlobQuotaTest, IsBlobQuotaErrorDetectsQuotaExceeded) {
  const Error quota = AppError::Blob(Err::Blob::QuotaExceeded, "Small file quota exceeded");
  EXPECT_TRUE(IsBlobQuotaError(quota));
  EXPECT_FALSE(IsBlobQuotaError(AppError::Network(Err::Network::HttpError, "HTTP 500")));
}

TEST(BlobQuotaTest, PlanOldestRelayBlobDeletionSkipsProtectedBlob) {
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

TEST(BlobQuotaTest, FreeOldestRelayBlobSlotDeletesOldestRemoteOnly) {
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

RelayBlobRecord MakeTiered(const std::string& blob_id, const std::string& created_at, const BlobTier tier,
                           const BlobPurpose purpose = BlobPurpose::File,
                           const BlobStatus status = BlobStatus::Retained) {
  RelayBlobRecord record = MakeBlob(blob_id, created_at, purpose);
  record.tier = tier;
  record.status = status;
  return record;
}

TEST(BlobQuotaTest, TierComesFromTheQuotaTextOfTheError) {
  EXPECT_EQ(BlobQuotaTierOf(AppError::Blob(Err::Blob::QuotaExceeded, "HTTP 429: Small file quota exceeded")),
            BlobTier::Small);
  EXPECT_EQ(BlobQuotaTierOf(AppError::Blob(Err::Blob::QuotaExceeded, "HTTP 429: Large file storage quota exceeded")),
            BlobTier::Large);
  EXPECT_FALSE(BlobQuotaTierOf(AppError::Blob(Err::Blob::QuotaExceeded, "HTTP 429")).has_value());
}

TEST(BlobQuotaTest, OldestIsChosenInTheFullTierOnlyAndNeverAnIconOrAnUploadInFlight) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeTiered("old-small", "2026-01-01T00:00:00.000Z", BlobTier::Small),
                  MakeTiered("old-large", "2026-02-01T00:00:00.000Z", BlobTier::Large),
                  MakeTiered("old-large-icon", "2026-01-15T00:00:00.000Z", BlobTier::Large, BlobPurpose::Icon),
                  MakeTiered("older-large-pending", "2026-01-02T00:00:00.000Z", BlobTier::Large, BlobPurpose::File,
                             BlobStatus::Pending),
                  MakeTiered("new-large", "2026-03-01T00:00:00.000Z", BlobTier::Large)};
  listed.usage.small_count = 100;
  blob.SetListResult(listed);

  auto large = PlanOldestRelayBlobDeletion(blob, "relay:u", "", BlobTier::Large);
  ASSERT_TRUE(static_cast<bool>(large));
  EXPECT_EQ(large->blob_to_delete.blob_id, "old-large") << "the oldest of the large tier, not of all";
  EXPECT_EQ(large->full_tier, BlobTier::Large);

  auto small = PlanOldestRelayBlobDeletion(blob, "relay:u", "", BlobTier::Small);
  ASSERT_TRUE(static_cast<bool>(small));
  EXPECT_EQ(small->blob_to_delete.blob_id, "old-small");
  EXPECT_EQ(small->full_tier, BlobTier::Small);
}

TEST(BlobQuotaTest, NothingToDeleteInTheFullTierIsAnErrorNotAWrongDeletion) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeTiered("only-small", "2026-01-01T00:00:00.000Z", BlobTier::Small),
                  MakeTiered("icon", "2026-01-01T00:00:00.000Z", BlobTier::Large, BlobPurpose::Icon)};
  blob.SetListResult(listed);
  auto plan = PlanOldestRelayBlobDeletion(blob, "relay:u", "", BlobTier::Large);
  ASSERT_FALSE(static_cast<bool>(plan));
  EXPECT_EQ(plan.error().code, static_cast<int32_t>(Err::Blob::NothingToDelete));
  EXPECT_TRUE(blob.DeletedBlobIds().empty());
}

TEST(BlobQuotaTest, DeleteRelayBlobDeletesExactlyTheIdGiven) {
  MockBlobClient blob;
  BlobListResult listed;
  listed.blobs = {MakeTiered("confirmed", "2026-03-01T00:00:00.000Z", BlobTier::Small),
                  MakeTiered("older-now", "2026-01-01T00:00:00.000Z", BlobTier::Small)};
  blob.SetListResult(listed);
  ASSERT_TRUE(static_cast<bool>(DeleteRelayBlob(blob, "relay:u", "confirmed")));
  ASSERT_EQ(blob.DeletedBlobIds().size(), 1u);
  EXPECT_EQ(blob.DeletedBlobIds().front(), "confirmed");
}

TEST(BlobQuotaTest, EmptyRelayUserIdRejected) {
  MockBlobClient blob;
  auto plan = PlanOldestRelayBlobDeletion(blob, "");
  ASSERT_FALSE(static_cast<bool>(plan));
  EXPECT_NE(plan.error().message.find("Register"), std::string::npos);
}

TEST(BlobQuotaTest, UploadRelayBlobBytesSurfacesQuotaErrorFromPresign) {
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
