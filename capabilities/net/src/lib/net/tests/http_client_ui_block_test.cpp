// A blocking HttpClient request made on the UI thread leaves one `net.ui_block` Diag line and metric
// (DEV/PLAN-unlock-gate-early-dismiss.md B). The route is the path's first two segments only, as a listed word: never
// the host, the query, a later segment or an id. No probe (node tools, tests) or a probe that says "not the UI": no line.
#include "common/Diag.h"
#include "common/Metrics.h"
#include "common/tests/LocalHttpServer.h"
#include "lib/net/HttpClient.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pbr {
namespace {

struct LineCapture : logging::Handler {
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mutex);
    lines.push_back(message);
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mutex);
    return std::exchange(lines, {});
  }
  std::mutex mutex;
  std::vector<std::string> lines;
};

/** The channels' loggers keep their handlers: one capture each for the whole binary. */
LineCapture& DiagCapture() {
  static const std::shared_ptr<LineCapture> capture = [] {
    auto made = std::make_shared<LineCapture>();
    DiagLog().addHandler(made);
    return made;
  }();
  return *capture;
}
LineCapture& MetricsCapture() {
  static const std::shared_ptr<LineCapture> capture = [] {
    auto made = std::make_shared<LineCapture>();
    MetricsLog().addHandler(made);
    return made;
  }();
  return *capture;
}

std::vector<std::string> UiBlockLines(LineCapture& capture) {
  std::vector<std::string> out;
  for (std::string& line : capture.Take()) {
    if (line.find("event=net.ui_block") != std::string::npos) {
      out.push_back(std::move(line));
    }
  }
  return out;
}

class HttpClientUiBlockTest : public ::testing::Test {
protected:
  void SetUp() override {
    (void)DiagCapture().Take();
    (void)MetricsCapture().Take();
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_http_ui_block_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  test::LocalHttpServer server_;
  std::filesystem::path dir_;
};

TEST(HttpClientUiBlockRouteTest, KeepsOnlyTheFirstTwoPathSegments) {
  EXPECT_EQ(HttpClient::UiBlockRoute("https://relay.example/v1/users/by-account/account:abc123?t=secret#x"),
            "/v1/users");
  EXPECT_EQ(HttpClient::UiBlockRoute("https://www.brief.global/api/relay/v1/devices/register"), "/api/relay");
  EXPECT_EQ(HttpClient::UiBlockRoute("http://127.0.0.1:8080/mcp"), "/mcp");
  EXPECT_EQ(HttpClient::UiBlockRoute("https://h.example"), "");
  EXPECT_EQ(HttpClient::UiBlockRoute("https://h.example?q=/v1/users"), "") << "a query is not a path";
  EXPECT_EQ(HttpClient::UiBlockRoute("https://user@h.example//v1//users/x"), "/v1/users");
  EXPECT_EQ(HttpClient::UiBlockRoute("/v1/inbox/poll?since=1"), "/v1/inbox");
}

TEST_F(HttpClientUiBlockTest, OnTheUiThreadAGetLeavesTheRouteAndNoMore) {
  const HttpClient::ScopedUiThreadProbeForTest on_ui([] { return true; });
  const std::string id = "account:4f1c2b9e7a6d5c3b";
  (void)HttpClient::Get(server_.Url() + "/v1/users/by-account/" + id + "?token=Secret123");

  const std::vector<std::string> diag = UiBlockLines(DiagCapture());
  ASSERT_EQ(diag.size(), 1u);
  EXPECT_NE(diag[0].find("route=v1.users"), std::string::npos) << diag[0];
  EXPECT_NE(diag[0].find(" ms="), std::string::npos) << diag[0];
  for (const char* leak : {"by-account", "4f1c2b9e7a6d5c3b", "account:", "127.0.0.1", "token", "Secret123"}) {
    EXPECT_EQ(diag[0].find(leak), std::string::npos) << "leaked " << leak << ": " << diag[0];
  }
  const std::vector<std::string> metrics = UiBlockLines(MetricsCapture());
  ASSERT_EQ(metrics.size(), 1u);
  EXPECT_NE(metrics[0].find(" ms="), std::string::npos) << metrics[0];
  EXPECT_EQ(metrics[0].find("by-account"), std::string::npos);
}

TEST_F(HttpClientUiBlockTest, ARouteNotListedIsWrittenOther) {
  const HttpClient::ScopedUiThreadProbeForTest on_ui([] { return true; });
  (void)HttpClient::Get(server_.Url() + "/alice-photos/8a7b6c5d/x.jpg");
  const std::vector<std::string> diag = UiBlockLines(DiagCapture());
  ASSERT_EQ(diag.size(), 1u);
  EXPECT_NE(diag[0].find("route=other"), std::string::npos) << diag[0];
  EXPECT_EQ(diag[0].find("alice"), std::string::npos) << diag[0];
}

TEST_F(HttpClientUiBlockTest, EveryEntryPointIsRecorded) {
  const HttpClient::ScopedUiThreadProbeForTest on_ui([] { return true; });
  const std::string upload = (dir_ / "upload.bin").string();
  {
    std::ofstream out(upload, std::ios::binary);
    out << "0123456789";
  }
  (void)HttpClient::Get(server_.Url() + "/v1/mesh/nodes");
  (void)HttpClient::Post(server_.Url() + "/v1/register/start", "{}");
  (void)HttpClient::PutFile(server_.Url() + "/v1/blobs/put", upload);
  (void)HttpClient::GetToFile(server_.Url() + "/v1/files/abc/download", (dir_ / "down.bin").string(), 1024);
  const std::vector<std::string> diag = UiBlockLines(DiagCapture());
  ASSERT_EQ(diag.size(), 4u);
  EXPECT_NE(diag[0].find("route=v1.mesh"), std::string::npos) << diag[0];
  EXPECT_NE(diag[1].find("route=v1.register"), std::string::npos) << diag[1];
  EXPECT_NE(diag[2].find("route=v1.blobs"), std::string::npos) << diag[2];
  EXPECT_NE(diag[3].find("route=v1.files"), std::string::npos) << diag[3];
  EXPECT_EQ(UiBlockLines(MetricsCapture()).size(), 4u);
}

TEST_F(HttpClientUiBlockTest, NoProbeOrNotTheUiThreadWritesNothing) {
  (void)HttpClient::Get(server_.Url() + "/v1/users/x"); // no probe: node tools, tests
  {
    const HttpClient::ScopedUiThreadProbeForTest off_ui([] { return false; });
    (void)HttpClient::Post(server_.Url() + "/v1/users/x", "{}");
  }
  EXPECT_TRUE(UiBlockLines(DiagCapture()).empty());
  EXPECT_TRUE(UiBlockLines(MetricsCapture()).empty());
}

} // namespace
} // namespace pbr
