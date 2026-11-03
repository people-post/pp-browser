#include "lib/net/BlobQuota.h"
#include "lib/net/BlobClient.h"
#include "foundation/error/AppError.h"
#include "lib/net/testing/MockOrgBackendClients.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

TEST(BlobQuotaTest, IsBlobQuotaErrorDetectsQuotaExceeded) {
  const Error quota = AppError::Blob(Err::Blob::QuotaExceeded, "Small file quota exceeded");
  EXPECT_TRUE(IsBlobQuotaError(quota));
  EXPECT_FALSE(IsBlobQuotaError(AppError::Network(Err::Network::HttpError, "HTTP 500")));
}

TEST(BlobQuotaTest, EmptyRelayUserIdRejected) {
  MockBlobClient blob;
  auto plan = PlanOldestRelayFileDeletion(blob, "", 1);
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
