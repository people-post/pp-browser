#include "feature/ai/tools/McpToolAdapter.h"

#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

namespace {

std::string ToolResultJsonOfSize(const size_t result_size) {
  pbr::Object text_block;
  text_block.set("type", "text");
  text_block.set("text", "");
  pbr::Object result;
  result.set("content", pbr::ArrayValue({pbr::ObjectValue(text_block)}));

  const size_t overhead = pbr::DumpJson(result).size();
  text_block.set("text", std::string(result_size - overhead, 'x'));
  result.set("content", pbr::ArrayValue({pbr::ObjectValue(std::move(text_block))}));
  return pbr::DumpJson(result);
}

class McpHttpPostReset {
public:
  ~McpHttpPostReset() { pbr::McpClient::SetHttpPost({}); }
};

} // namespace

TEST(McpToolAdapterTest, BoundsSerializedToolResultsAtConfiguredLimit) {
  McpHttpPostReset reset;
  std::string result_json = ToolResultJsonOfSize(pbr::kMaxMcpToolResultBytes);
  ASSERT_EQ(result_json.size(), pbr::kMaxMcpToolResultBytes);

  pbr::McpClient::SetHttpPost([&result_json](const std::string&, const std::string& request,
                                              const std::map<std::string, std::string>&) -> pbr::Roe<pbr::HttpResponse> {
    if (request.find("tools/list") != std::string::npos) {
      return pbr::HttpResponse{.status_code = 200,
                               .body = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"large_result","description":"","inputSchema":{"type":"object"}}]}})"};
    }
    return pbr::HttpResponse{.status_code = 200,
                             .body = R"({"jsonrpc":"2.0","id":2,"result":)" + result_json + "}"};
  });

  pbr::McpClient client;
  ASSERT_TRUE(client.StartHttp("http://mcp.test"));
  const auto tools = pbr::McpToolAdapter::ListTools(client);
  ASSERT_EQ(tools.size(), 1u);

  auto at_limit = tools.front().execute(pbr::Object{});
  ASSERT_TRUE(at_limit) << at_limit.error().message;
  EXPECT_EQ(at_limit->size(), pbr::kMaxMcpToolResultBytes);

  result_json = ToolResultJsonOfSize(pbr::kMaxMcpToolResultBytes + 1);
  const auto over_limit = tools.front().execute(pbr::Object{});
  ASSERT_FALSE(over_limit);
  EXPECT_EQ(over_limit.error().message,
            "MCP tool result exceeds limit of " + std::to_string(pbr::kMaxMcpToolResultBytes) + " bytes");
}

// PB-055: a server's own `readOnlyHint` makes a tool "read" (runs without asking) only for the promoted
// server, which is ours. A custom server's tool is at least "write": the user is asked the first time and
// may choose Always allow. A destructive-sounding name stays destructive either way.
TEST(McpToolAdapterTest, ReadOnlyHintIsTrustedOnlyFromThePromotedServer) {
  McpHttpPostReset reset;
  pbr::McpClient::SetHttpPost([](const std::string&, const std::string&,
                                 const std::map<std::string, std::string>&) -> pbr::Roe<pbr::HttpResponse> {
    return pbr::HttpResponse{
        .status_code = 200,
        .body = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[)"
                R"({"name":"lookup","description":"","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}},)"
                R"({"name":"delete_all","description":"","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true}})"
                R"(]}})"};
  });
  pbr::McpClient client;
  ASSERT_TRUE(client.StartHttp("http://mcp.test"));

  const auto custom = pbr::McpToolAdapter::ListTools(client);
  ASSERT_EQ(custom.size(), 2u);
  EXPECT_EQ(custom[0].meta.risk, "write");
  EXPECT_TRUE(custom[0].meta.mutating);
  EXPECT_EQ(custom[1].meta.risk, "destructive");

  const auto promoted = pbr::McpToolAdapter::ListTools(client, {.trust_server_annotations = true});
  ASSERT_EQ(promoted.size(), 2u);
  EXPECT_EQ(promoted[0].meta.risk, "read");
  EXPECT_FALSE(promoted[0].meta.mutating);
  EXPECT_EQ(promoted[1].meta.risk, "destructive");
}
