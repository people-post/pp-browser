#include "domain/net/BlobClient.h"
#include "domain/net/OrgBackendClientFactory.h"
#include "domain/net/OrgBackendClientsImpl.h"

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

TEST(BlobClientTest, BlobPurposeWireRoundTrip) {
  EXPECT_EQ(BlobPurposeToWire(BlobPurpose::Icon), "icon");
  EXPECT_EQ(BlobPurposeToWire(BlobPurpose::File), "file");
  ASSERT_TRUE(BlobPurposeFromWire("icon").has_value());
  EXPECT_EQ(*BlobPurposeFromWire("icon"), BlobPurpose::Icon);
  EXPECT_FALSE(BlobPurposeFromWire("other").has_value());
}

} // namespace
} // namespace pbr
