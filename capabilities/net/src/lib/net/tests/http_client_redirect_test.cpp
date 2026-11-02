#include "common/tests/ScriptedHttpServer.h"
#include "lib/net/HttpClient.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace pbr {
namespace {

using test::ScriptedHttpServer;

// The relay's download answers 302 to a 60-second S3 URL. Following it is opt-in, at most two hops, and every
// hop meets the rules the first one does: https + public addresses when restricted, and the size cap.
class HttpClientRedirectTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_http_redirect_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    HttpClient::SetFirstHopLocalForTest(false);
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** `/a` → 302 to `location`; `/b` → 200 "payload"; `/big` → 200 with 64 KiB. */
  void Script(const std::string& location) {
    server_.SetHandler([location](const ScriptedHttpServer::Request& request) {
      if (request.target == "/a") {
        return ScriptedHttpServer::Reply{.status = 302, .headers = {{"Location", location}}, .body = "moved"};
      }
      if (request.target == "/b") {
        return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = "payload"};
      }
      if (request.target == "/big") {
        return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = std::string(64 * 1024, 'x')};
      }
      return ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = ""};
    });
  }
  std::string Url(const std::string& path) const { return server_.Origin() + path; }
  std::string Dest() const { return (dir_ / "out.bin").string(); }
  int RequestsFor(const std::string& target) const {
    int count = 0;
    for (const auto& request : server_.Requests()) {
      count += request.target == target ? 1 : 0;
    }
    return count;
  }

  std::filesystem::path dir_;
  ScriptedHttpServer server_;
};

TEST_F(HttpClientRedirectTest, OffByDefaultA302IsTheAnswer) {
  Script(Url("/b"));
  auto got = HttpClient::Get(Url("/a"));
  ASSERT_TRUE(got) << got.error().message;
  EXPECT_EQ(got->status_code, 302);
  EXPECT_EQ(got->redirect_count, 0);
  auto file = HttpClient::GetToFile(Url("/a"), Dest(), 1024);
  ASSERT_TRUE(file) << file.error().message;
  EXPECT_EQ(file->status_code, 302);
  EXPECT_EQ(file->bytes, 0u);
  EXPECT_FALSE(std::filesystem::exists(Dest()));
  EXPECT_EQ(RequestsFor("/b"), 0);
}

TEST_F(HttpClientRedirectTest, FollowedA302FetchesTheSecondAddress) {
  Script(Url("/b"));
  auto got = HttpClient::Get(Url("/a"), {}, kMaxHttpClientBodyBytes, HttpTimeout{}, false,
                             /*follow_redirects=*/true);
  ASSERT_TRUE(got) << got.error().message;
  EXPECT_EQ(got->status_code, 200);
  EXPECT_EQ(got->body, "payload");
  EXPECT_EQ(got->redirect_count, 1);

  auto file = HttpClient::GetToFile(Url("/a"), Dest(), 1024, {}, false, {}, /*follow_redirects=*/true);
  ASSERT_TRUE(file) << file.error().message;
  EXPECT_EQ(file->status_code, 200);
  EXPECT_EQ(file->bytes, 7u);
  EXPECT_EQ(file->redirect_count, 1);
  EXPECT_EQ(std::filesystem::file_size(Dest()), 7u) << "only the final body is written, not the 302's";
}

TEST_F(HttpClientRedirectTest, TwoHopsAreFollowedAThirdFails) {
  server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
    const auto hop = [&](const std::string& next) {
      return ScriptedHttpServer::Reply{.status = 302, .headers = {{"Location", Url(next)}}, .body = ""};
    };
    if (request.target == "/r1") {
      return hop("/r2");
    }
    if (request.target == "/r2") {
      return hop("/r3");
    }
    if (request.target == "/r3") {
      return hop("/ok");
    }
    return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = "ok"};
  });
  auto two = HttpClient::Get(Url("/r2"), {}, kMaxHttpClientBodyBytes, HttpTimeout{}, false, true);
  ASSERT_TRUE(two) << two.error().message;
  EXPECT_EQ(two->body, "ok");
  EXPECT_EQ(two->redirect_count, 2);

  EXPECT_FALSE(HttpClient::Get(Url("/r1"), {}, kMaxHttpClientBodyBytes, HttpTimeout{}, false, true));
  EXPECT_FALSE(HttpClient::GetToFile(Url("/r1"), Dest(), 1024, {}, false, {}, true));
  EXPECT_FALSE(std::filesystem::exists(Dest()));
}

TEST_F(HttpClientRedirectTest, TheSizeCapHoldsOnTheRedirectedBody) {
  Script(Url("/big"));
  auto got = HttpClient::Get(Url("/a"), {}, size_t{1024}, HttpTimeout{}, false, true);
  ASSERT_FALSE(got);
  EXPECT_NE(got.error().message.find("exceeds"), std::string::npos) << got.error().message;

  auto file = HttpClient::GetToFile(Url("/a"), Dest(), 1024, {}, false, {}, true);
  ASSERT_FALSE(file);
  EXPECT_FALSE(std::filesystem::exists(Dest())) << "an overrun after a redirect leaves nothing";
}

// Restricted fetches (a peer-sent URL): only the first hop is let through to the loopback test server; the
// redirect target is held to https + public addresses.
TEST_F(HttpClientRedirectTest, RestrictedRedirectToPlainHttpIsRefused) {
  HttpClient::SetFirstHopLocalForTest(true);
  Script(Url("/b"));
  auto got = HttpClient::Get(Url("/a"), {}, kMaxHttpClientBodyBytes, HttpTimeout{}, /*restrict=*/true, true);
  ASSERT_FALSE(got);
  EXPECT_EQ(RequestsFor("/a"), 1) << "the first hop was served";
  EXPECT_EQ(RequestsFor("/b"), 0) << "the http:// second hop never went out";
  EXPECT_EQ(got.error().message.find("/b"), std::string::npos) << "the target is not in the error";

  auto file = HttpClient::GetToFile(Url("/a"), Dest(), 1024, {}, true, {}, true);
  ASSERT_FALSE(file);
  EXPECT_EQ(RequestsFor("/b"), 0);
}

TEST_F(HttpClientRedirectTest, RestrictedRedirectToLoopbackHttpsIsRefusedByAddress) {
  HttpClient::SetFirstHopLocalForTest(true);
  Script("https://127.0.0.1:" + std::to_string(server_.Port()) + "/b");
  auto got = HttpClient::Get(Url("/a"), {}, kMaxHttpClientBodyBytes, HttpTimeout{.total_s = 10}, true, true);
  ASSERT_FALSE(got);
  EXPECT_EQ(got.error().message.find("Unsupported protocol"), std::string::npos) << got.error().message;
  EXPECT_EQ(server_.Connections(), 1) << "the second hop's socket was refused before it connected";

  auto file = HttpClient::GetToFile(Url("/a"), Dest(), 1024, {}, true, {}, true);
  ASSERT_FALSE(file);
  EXPECT_EQ(server_.Connections(), 2);
}

TEST_F(HttpClientRedirectTest, RestrictedRedirectToAPrivateAddressIsRefusedAtOnce) {
  HttpClient::SetFirstHopLocalForTest(true);
  Script("https://10.0.0.1/b");
  const auto started = std::chrono::steady_clock::now();
  auto got = HttpClient::Get(Url("/a"), {}, kMaxHttpClientBodyBytes, HttpTimeout{.total_s = 20}, true, true);
  ASSERT_FALSE(got);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5))
      << "refused by the socket check, not a connect timeout";
  EXPECT_EQ(got.error().message.find("Unsupported protocol"), std::string::npos) << got.error().message;
  EXPECT_EQ(RequestsFor("/a"), 1);
}

TEST_F(HttpClientRedirectTest, WithoutTheTestSwitchARestrictedFirstHopToLoopbackIsRefused) {
  Script(Url("/b"));
  EXPECT_FALSE(HttpClient::Get(Url("/a"), {}, kMaxHttpClientBodyBytes, HttpTimeout{}, true, true));
  EXPECT_EQ(server_.Connections(), 0);
}

} // namespace
} // namespace pbr
