#include "domain/ai/mcp/McpClient.h"
#include "domain/ai/mcp/McpRuntime.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <cstring>
#include <string>
#include "common/PbrCompat.h"

namespace {

// Feeds a fixed byte string in chunks of `chunk`, then EOF.
struct ScriptedReader {
  std::string bytes;
  size_t chunk = 7;
  size_t pos = 0;
  int operator()(char* out, size_t size) {
    const size_t n = std::min({size, chunk, bytes.size() - pos});
    std::memcpy(out, bytes.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
};

} // namespace

// PB-056: the stdio transport takes the response whose id matches the request; notifications and other
// lines in between are skipped, and bytes after the response stay for the next call.
TEST(McpStdioFramingTest, TakesTheLineWithTheMatchingIdAndKeepsTheRest) {
  ScriptedReader reader{R"({"jsonrpc":"2.0","method":"notifications/progress","params":{}})"
                        "\n"
                        R"({"jsonrpc":"2.0","id":41,"result":{"stale":true}})"
                        "\n"
                        R"({"jsonrpc":"2.0","id":42,"result":{"ok":true}})"
                        "\n"
                        R"({"jsonrpc":"2.0","id":43,"result":{}})"
                        "\n",
                        /*chunk=*/1024};
  std::string carry;
  auto result = pbr::McpClient::ReadStdioResponse([&](char* out, size_t size) { return reader(out, size); }, 42, carry);
  ASSERT_TRUE(static_cast<bool>(result)) << result.error().message;
  const pbr::Object* payload = result->getObject("result");
  ASSERT_NE(payload, nullptr);
  EXPECT_TRUE(payload->getIf<bool>("ok").value_or(false));
  EXPECT_EQ(carry, R"({"jsonrpc":"2.0","id":43,"result":{}})" "\n");
}

TEST(McpStdioFramingTest, AnOverlongLineIsAnErrorNotAnEndlessRead) {
  size_t served = 0;
  auto endless = [&](char* out, size_t size) {
    std::memset(out, 'a', size);
    served += size;
    return static_cast<int>(size);
  };
  std::string carry;
  auto result = pbr::McpClient::ReadStdioResponse(endless, 1, carry);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("too long"), std::string::npos) << result.error().message;
  EXPECT_LE(served, pbr::kMaxMcpResponseBytes + 64 * 1024);
}

TEST(McpStdioFramingTest, TimeoutAndEofAreErrors) {
  std::string carry;
  auto timed_out = [](char*, size_t) { return pbr::os::OsProcessPipe::kReadTimedOut; };
  auto result = pbr::McpClient::ReadStdioResponse(timed_out, 1, carry);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("timed out"), std::string::npos) << result.error().message;

  ScriptedReader eof{R"({"jsonrpc":"2.0","id":1,"resu)"};
  auto closed = pbr::McpClient::ReadStdioResponse([&](char* out, size_t size) { return eof(out, size); }, 1, carry);
  ASSERT_FALSE(static_cast<bool>(closed));
  EXPECT_NE(closed.error().message.find("closed"), std::string::npos) << closed.error().message;
}

TEST(McpStdioFramingTest, TooManyForeignLinesIsAnError) {
  std::string flood;
  for (size_t i = 0; i < pbr::kMaxMcpSkippedLines + 1; ++i) {
    flood += R"({"jsonrpc":"2.0","method":"notifications/x"})" "\n";
  }
  ScriptedReader reader{flood, 1024};
  std::string carry;
  auto result = pbr::McpClient::ReadStdioResponse([&](char* out, size_t size) { return reader(out, size); }, 5, carry);
  ASSERT_FALSE(static_cast<bool>(result));
}

// A server that cannot be started is a failure the user sees, never a silent switch to the mock server.
TEST(McpStdioFramingTest, AFailedStartIsNotAMock) {
  pbr::McpClient client;
  EXPECT_FALSE(client.Start("/nonexistent/pp-mcp-server-that-does-not-exist"));
  EXPECT_FALSE(client.IsRunning());
}

// An HTTP MCP server is stateless per request: a failed initialize at startup (the phone's network is often
// not up yet when the app launches) must not drop it for the rest of the session; the next request simply
// tries again. A stdio server that fails initialize is still dropped (#300): its process is not trustworthy.
TEST(McpRuntimeTest, HttpServerSurvivesAFailedInitialize) {
  struct Reset {
    ~Reset() { pbr::McpClient::SetHttpPost({}); }
  } reset;
  int posts = 0;
  pbr::McpClient::SetHttpPost([&](const std::string&, const std::string&,
                                  const std::map<std::string, std::string>&, long) -> pbr::Roe<pbr::HttpResponse> {
    ++posts;
    return pbr::Error("HTTP request failed: Could not resolve hostname");
  });
  pbr::McpConfig config;
  config.id = "promoted";
  config.url = "https://mcp.test/mcp";
  config.enabled = true;
  pbr::McpClient client;
  EXPECT_TRUE(pbr::StartMcpClient(client, config));
  EXPECT_TRUE(client.IsRunning());
  EXPECT_GE(posts, 1);
}
