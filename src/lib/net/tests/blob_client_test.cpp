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

namespace {

Error Fail(const int status, const std::string& error_text) {
  const auto result = CheckBlobResponse(
      HttpResponse{.status_code = status, .body = error_text.empty() ? "oops" : R"({"error":")" + error_text + R"("})"});
  EXPECT_FALSE(static_cast<bool>(result)) << status;
  return result ? Error() : result.error();
}

int32_t CodeOf(const Error& err) {
  EXPECT_EQ(err.category, static_cast<int32_t>(ErrorCategory::Blob));
  return err.code;
}

} // namespace

TEST(BlobClientTest, CheckBlobResponseAcceptsSuccess) {
  EXPECT_TRUE(static_cast<bool>(CheckBlobResponse(HttpResponse{.status_code = 200, .body = "{}"})));
}

// The status table of the attachments plan, row by row; the statuses are the ones r_relay.ts /
// RelayBlobs.ts (blobFailureStatus) can produce.
TEST(BlobClientTest, StatusTableMapsEveryRowToATypedError) {
  // Upload not enabled: 503 from the relay (400 from an older gateway), matched by its text.
  EXPECT_EQ(CodeOf(Fail(503, "Blob upload is not configured")), static_cast<int32_t>(Err::Blob::UploadNotConfigured));
  EXPECT_EQ(CodeOf(Fail(400, "Blob upload is not configured")), static_cast<int32_t>(Err::Blob::UploadNotConfigured));
  // 403: not the owner / not registered.
  EXPECT_EQ(CodeOf(Fail(403, "Blob not owned by caller")), static_cast<int32_t>(Err::Blob::UploadRejected));
  EXPECT_EQ(CodeOf(Fail(403, "Relay user not registered")), static_cast<int32_t>(Err::Blob::UploadRejected));
  // 404: the blob is gone (expired pending upload).
  EXPECT_EQ(CodeOf(Fail(404, "Blob not found")), static_cast<int32_t>(Err::Blob::Expired));
  // Too large: 413, and the relay's own 400 for an object over the large tier.
  EXPECT_EQ(CodeOf(Fail(413, "")), static_cast<int32_t>(Err::Blob::TooLarge));
  EXPECT_EQ(CodeOf(Fail(400, "File exceeds size limit")), static_cast<int32_t>(Err::Blob::TooLarge));
  // 429 is a full space only for the two quota texts.
  EXPECT_EQ(CodeOf(Fail(429, "Small file quota exceeded")), static_cast<int32_t>(Err::Blob::QuotaExceeded));
  EXPECT_EQ(CodeOf(Fail(429, "Large file storage quota exceeded")), static_cast<int32_t>(Err::Blob::QuotaExceeded));
  EXPECT_EQ(CodeOf(Fail(429, "quota")), static_cast<int32_t>(Err::Blob::QuotaExceeded));
  EXPECT_EQ(CodeOf(Fail(429, "Rate limit exceeded")), static_cast<int32_t>(Err::Blob::RateLimited));
  EXPECT_EQ(CodeOf(Fail(429, "Too many pending uploads")), static_cast<int32_t>(Err::Blob::RateLimited));
  // 5xx and the rest.
  EXPECT_EQ(CodeOf(Fail(503, "")), static_cast<int32_t>(Err::Blob::Unavailable));
  EXPECT_EQ(CodeOf(Fail(500, "")), static_cast<int32_t>(Err::Blob::Unavailable));
  EXPECT_EQ(CodeOf(Fail(400, "Unsupported content type")), static_cast<int32_t>(Err::Blob::Generic));
  EXPECT_EQ(CodeOf(Fail(401, "Invalid signature")), static_cast<int32_t>(Err::Blob::Generic));
}

TEST(BlobClientTest, FailureDetailKeepsStatusAndServerText) {
  const Error err = Fail(429, "Rate limit exceeded");
  EXPECT_EQ(err.message, "HTTP 429: Rate limit exceeded");
  EXPECT_EQ(Fail(500, "").message, "HTTP 500");
  // Server text is log detail only; a reworded gateway message must not hit the "not configured" hint.
  const Error reworded = Fail(400, "Icon CDN not configured");
  EXPECT_EQ(reworded.code, static_cast<int32_t>(Err::Blob::Generic));
  EXPECT_EQ(reworded.message, "HTTP 400: Icon CDN not configured");
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
