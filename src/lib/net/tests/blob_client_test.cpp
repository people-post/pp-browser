#include "lib/net/BlobClient.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/net/OrgBackendClientFactory.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(BlobClientTest, MockUploadRelayBlobBytesPresignPutRetain) {
  MockBlobClient blob;
  const std::string body = "encrypted-bytes";
  auto uploaded = UploadRelayBlobBytes(blob, "relay:u1", "application/octet-stream", BlobPurpose::File, body);
  ASSERT_TRUE(static_cast<bool>(uploaded));
  EXPECT_FALSE(uploaded.value().public_url.empty());
  ASSERT_EQ(blob.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob.UploadedBodies().front(), body);
  ASSERT_EQ(blob.RetainedBlobIds().size(), 1u);
  EXPECT_EQ(blob.RetainedBlobIds().front(), uploaded.value().blob_id);
}

TEST(BlobClientTest, OrgBackendClientFactoryCreatesBlobClient) {
  AppConfig config;
  config.registration.base_url = "https://registration.example";
  auto clients = CreateOrgBackendClients(config);
  ASSERT_TRUE(static_cast<bool>(clients.blob));
}

TEST(BlobClientTest, CheckBlobResponseMapsUploadNotConfigured) {
  const auto result = CheckBlobResponse(HttpResponse{.status_code = 400, .body = R"({"error":"Blob upload is not configured"})"});
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_EQ(result.error().category, static_cast<int32_t>(ErrorCategory::Blob));
  EXPECT_EQ(result.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfigured));
  EXPECT_EQ(result.error().message, "Blob upload is not configured");
}

TEST(BlobClientTest, CheckBlobResponseKeepsQuotaAndSuccess) {
  EXPECT_TRUE(static_cast<bool>(CheckBlobResponse(HttpResponse{.status_code = 200, .body = "{}"})));
  const auto quota = CheckBlobResponse(HttpResponse{.status_code = 429, .body = R"({"error":"quota"})"});
  ASSERT_FALSE(static_cast<bool>(quota));
  EXPECT_EQ(quota.error().code, static_cast<int32_t>(Err::Blob::QuotaExceeded));
  const auto other = CheckBlobResponse(HttpResponse{.status_code = 500, .body = "oops"});
  ASSERT_FALSE(static_cast<bool>(other));
  EXPECT_EQ(other.error().code, static_cast<int32_t>(Err::Blob::Generic));
  EXPECT_EQ(other.error().message, "HTTP request failed with status 500");
  // Server text is log detail only; a reworded gateway message must not hit the "not configured" hint.
  const auto reworded = CheckBlobResponse(HttpResponse{.status_code = 400, .body = R"({"error":"Icon CDN not configured"})"});
  ASSERT_FALSE(static_cast<bool>(reworded));
  EXPECT_EQ(reworded.error().code, static_cast<int32_t>(Err::Blob::Generic));
  EXPECT_EQ(reworded.error().message, "Icon CDN not configured");
}

TEST(BlobClientTest, BlobPurposeWireRoundTrip) {
  EXPECT_EQ(BlobPurposeToWire(BlobPurpose::Icon), "icon");
  EXPECT_EQ(BlobPurposeToWire(BlobPurpose::File), "file");
  ASSERT_TRUE(BlobPurposeFromWire("icon").has_value());
  EXPECT_EQ(*BlobPurposeFromWire("icon"), BlobPurpose::Icon);
  EXPECT_FALSE(BlobPurposeFromWire("other").has_value());
}

} // namespace
} // namespace pbr
