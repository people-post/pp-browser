#include "common/tests/ScriptedHttpServer.h"
#include "foundation/tls/ContentSha256Policy.h"
#include "lib/net/HttpClient.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
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

// The header goes out only with the process-wide switch on; the pure WithContentSha256 above is switch-independent.
class HttpClientContentSha256WireTest : public ::testing::Test {
protected:
  void SetUp() override {
    server_.SetHandler([](const test::ScriptedHttpServer::Request&) {
      return test::ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = "ok"};
    });
  }
  /** Whether the last request the server saw carried the header (any letter case). */
  bool LastRequestHasHeader() const {
    const auto requests = server_.Requests();
    EXPECT_FALSE(requests.empty());
    std::string head = requests.back().head;
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head.find("\r\nx-amz-content-sha256: ") != std::string::npos;
  }
  std::string LastRequestHead() const { return server_.Requests().back().head; }

  test::ScriptedHttpServer server_;
};

TEST_F(HttpClientContentSha256WireTest, OffByDefaultNothingIsAdded) {
  const ScopedSendContentSha256ForTest off(false);
  ASSERT_TRUE(HttpClient::Post(server_.Origin() + "/p", "abc"));
  EXPECT_FALSE(LastRequestHasHeader());
  ASSERT_TRUE(HttpClient::Put(server_.Origin() + "/p", "abc"));
  EXPECT_FALSE(LastRequestHasHeader());
}

TEST_F(HttpClientContentSha256WireTest, OnPostAndPutCarryTheHash) {
  const ScopedSendContentSha256ForTest on(true);
  ASSERT_TRUE(HttpClient::Post(server_.Origin() + "/p", "abc"));
  EXPECT_NE(LastRequestHead().find(std::string("x-amz-content-sha256: ") + kAbcSha), std::string::npos);
  ASSERT_TRUE(HttpClient::Put(server_.Origin() + "/p", ""));
  EXPECT_NE(LastRequestHead().find(std::string("x-amz-content-sha256: ") + kEmptySha), std::string::npos);
}

TEST_F(HttpClientContentSha256WireTest, OnCallerValueIsNotOverwritten) {
  const ScopedSendContentSha256ForTest on(true);
  ASSERT_TRUE(HttpClient::Post(server_.Origin() + "/p", "abc", {{"X-Amz-Content-Sha256", "caller"}}));
  const std::string head = LastRequestHead();
  EXPECT_NE(head.find("X-Amz-Content-Sha256: caller"), std::string::npos);
  EXPECT_EQ(head.find(kAbcSha), std::string::npos);
}

TEST_F(HttpClientContentSha256WireTest, OnPresignedOptOutStillWins) {
  const ScopedSendContentSha256ForTest on(true);
  ASSERT_TRUE(HttpClient::Put(server_.Origin() + "/p", "abc", {}, kMaxHttpClientBodyBytes, HttpTimeout{}, false,
                              /*add_content_sha256=*/false));
  EXPECT_FALSE(LastRequestHasHeader());
}

TEST(ContentSha256PolicyTest, DefaultsOffAndTheScopeRestoresIt) {
  EXPECT_FALSE(SendContentSha256());
  {
    const ScopedSendContentSha256ForTest on(true);
    EXPECT_TRUE(SendContentSha256());
  }
  EXPECT_FALSE(SendContentSha256());
}

} // namespace
} // namespace pbr
