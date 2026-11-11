#include "lib/net/HttpClient.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace pbr {
namespace {

constexpr char kEmptySha[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
constexpr char kAbcSha[] = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

TEST(HttpClientContentSha256Test, PostAndPutCarryTheBodyHash) {
  for (const char* method : {"POST", "PUT"}) {
    EXPECT_EQ(HttpClient::WithContentSha256(method, "abc", {}).at("x-amz-content-sha256"), kAbcSha) << method;
    EXPECT_EQ(HttpClient::WithContentSha256(method, "", {}).at("x-amz-content-sha256"), kEmptySha) << method;
  }
}

TEST(HttpClientContentSha256Test, GetIsUntouched) {
  const std::map<std::string, std::string> headers{{"Accept", "application/json"}};
  EXPECT_EQ(HttpClient::WithContentSha256("GET", "", headers), headers);
}

TEST(HttpClientContentSha256Test, CallerValueIsKeptInAnyCase) {
  const auto kept = HttpClient::WithContentSha256("POST", "abc", {{"X-Amz-Content-Sha256", "caller"}});
  ASSERT_EQ(kept.size(), 1U);
  EXPECT_EQ(kept.at("X-Amz-Content-Sha256"), "caller");
}

TEST(HttpClientContentSha256Test, OtherHeadersArePreserved) {
  const auto out = HttpClient::WithContentSha256("POST", "abc", {{"Content-Type", "application/json"}});
  EXPECT_EQ(out.at("Content-Type"), "application/json");
  EXPECT_EQ(out.size(), 2U);
}

} // namespace
} // namespace pbr
