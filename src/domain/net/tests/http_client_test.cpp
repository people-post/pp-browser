#include "common/PlatformLimits.h"
#include "domain/net/HttpClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

class HttpClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    response_dir_ = std::filesystem::temp_directory_path() /
                    ("pbr_http_client_test_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(response_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(response_dir_); }

  std::string WriteResponseFile(size_t bytes) {
    const std::filesystem::path path = response_dir_ / "response";
    std::ofstream out(path, std::ios::binary);
    out.seekp(static_cast<std::streamoff>(bytes - 1));
    out.put('\0');
    out.close();
    return "file://" + path.string();
  }

  std::filesystem::path response_dir_;
};

TEST_F(HttpClientTest, UsesDefaultResponseLimit) {
  EXPECT_EQ(pbr::kMaxHttpClientBodyBytes, 8U * 1024U * 1024U);

  const auto response = pbr::HttpClient::Get(WriteResponseFile(pbr::kMaxHttpClientBodyBytes));

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), pbr::kMaxHttpClientBodyBytes);

  const auto over_limit = pbr::HttpClient::Get(WriteResponseFile(pbr::kMaxHttpClientBodyBytes + 1));
  ASSERT_FALSE(over_limit);
  EXPECT_NE(over_limit.error().message.find("configured limit of 8388608 bytes"), std::string::npos);
}

TEST_F(HttpClientTest, HonorsExplicitResponseLimit) {
  const auto response = pbr::HttpClient::Get(WriteResponseFile(6), {}, 6);

  ASSERT_TRUE(response) << response.error().message;
  EXPECT_EQ(response->body.size(), 6U);
}

TEST_F(HttpClientTest, AbortsReadWhenResponseExceedsLimit) {
  const auto response = pbr::HttpClient::Get(WriteResponseFile(6), {}, 5);

  ASSERT_FALSE(response);
  EXPECT_NE(response.error().message.find("configured limit of 5 bytes"), std::string::npos);
}

} // namespace
