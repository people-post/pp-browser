#include "common/PlatformLimits.h"
#include "common/tests/LocalHttpServer.h"
#include "domain/net/HttpClient.h"

#include <gtest/gtest.h>

#include <string>

namespace {

class HttpClientTest : public ::testing::Test {
protected:
  std::string WriteResponse(const size_t bytes) {
    server_.SetResponse(std::string(bytes, '\0'));
    return server_.Url();
  }

  pbr::test::LocalHttpServer server_;
};

TEST_F(HttpClientTest, UsesDefaultResponseLimit) {
  EXPECT_EQ(pbr::kMaxHttpClientBodyBytes, 8U * 1024U * 1024U);

  auto response = pbr::HttpClient::Get(WriteResponse(pbr::kMaxHttpClientBodyBytes));

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), pbr::kMaxHttpClientBodyBytes);

  const auto over_limit = pbr::HttpClient::Get(WriteResponse(pbr::kMaxHttpClientBodyBytes + 1));
  ASSERT_FALSE(over_limit);
  EXPECT_NE(over_limit.error().message.find("configured limit of 8388608 bytes"), std::string::npos);
}

TEST_F(HttpClientTest, HonorsExplicitResponseLimit) {
  auto response = pbr::HttpClient::Get(WriteResponse(6), {}, 6);

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), 6U);
}

TEST_F(HttpClientTest, AbortsReadWhenResponseExceedsLimit) {
  const auto response = pbr::HttpClient::Get(WriteResponse(6), {}, 5);

  ASSERT_FALSE(response);
  EXPECT_NE(response.error().message.find("configured limit of 5 bytes"), std::string::npos);
}

} // namespace
