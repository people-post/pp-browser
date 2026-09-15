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

  const auto at_limit = tools.front().execute(pbr::Object{});
  ASSERT_TRUE(at_limit) << at_limit.error().message;
  EXPECT_EQ(at_limit->size(), pbr::kMaxMcpToolResultBytes);

  result_json = ToolResultJsonOfSize(pbr::kMaxMcpToolResultBytes + 1);
  const auto over_limit = tools.front().execute(pbr::Object{});
  ASSERT_FALSE(over_limit);
  EXPECT_EQ(over_limit.error().message,
            "MCP tool result exceeds limit of " + std::to_string(pbr::kMaxMcpToolResultBytes) + " bytes");
}
