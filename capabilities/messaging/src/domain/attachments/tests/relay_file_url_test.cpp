#include "domain/attachments/RelayFileUrl.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

constexpr const char kLink[] =
    "https://www.brief.global/api/relay/v1/files/0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e/download?t=AbCdEf0123456789_-xyz";

TEST(RelayFileUrlTest, BlobIdOfAShareLink) {
  EXPECT_EQ(RelayFileUrl::BlobIdOf(kLink), "0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("http://127.0.0.1:8080/api/relay/v1/files/abc-1/download?t=x"), "abc-1");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/api/relay/v1/files/abc-1/download"), "abc-1");
}

TEST(RelayFileUrlTest, AnythingElseHasNoBlobId) {
  EXPECT_EQ(RelayFileUrl::BlobIdOf(""), "");
  // An old CDN link of the /blobs relay.
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://cdn.example/pp/files/0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/api/relay/v1/files//download?t=x"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/api/relay/v1/files/abc/other?t=x"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/api/relay/v1/files/abc/download/more"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/api/relay/v1/files/a%2Fb/download?t=x"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("https://h/prefix/api/relay/v1/files/abc/download?t=x"), "");
  EXPECT_EQ(RelayFileUrl::BlobIdOf("/api/relay/v1/files/abc/download?t=x"), "") << "a path alone is not a link";
}

TEST(RelayFileUrlTest, RedactedDropsTheTokenAndKeepsThePath) {
  EXPECT_EQ(RelayFileUrl::Redacted(kLink),
            "https://www.brief.global/api/relay/v1/files/0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e/download");
  EXPECT_EQ(RelayFileUrl::Redacted("https://s3.example/obj?X-Amz-Signature=abc#frag"), "https://s3.example/obj");
  EXPECT_EQ(RelayFileUrl::Redacted("https://h/p#t=frag"), "https://h/p");
  EXPECT_EQ(RelayFileUrl::Redacted("https://h/p"), "https://h/p");
}

} // namespace
} // namespace pbr
