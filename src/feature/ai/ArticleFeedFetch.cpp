#include "feature/ai/ArticleFeedFetch.h"

#include "common/ValueJson.h"
#include "lib/ai/ArticleFeedBlocks.h"
#include "lib/ai/mcp/McpClient.h"
#include "lib/ai/mcp/McpRuntime.h"
#include "lib/net/HttpClient.h"

#include <map>

namespace pbr {

void WireMcpHttpPost() {
  McpClient::SetHttpPost([](const std::string& url, const std::string& body,
                            const std::map<std::string, std::string>& headers, const long timeout_s) {
    HttpTimeout timeout;
    if (timeout_s > 0) {
      timeout.total_s = timeout_s;
    }
    return HttpClient::Post(url, body, headers, kMaxHttpClientBodyBytes, timeout);
  });
}

Roe<std::string> FetchArticleFeed(const AppConfig& config, const Object& arguments) {
  const McpConfig server = ResolvePromotedMcp(config, Config::DefaultAppConfig());
  // Only an HTTP server: Home never starts a local MCP process of its own.
  if (!server.enabled || server.url.empty()) {
    return Error("no HTTP article feed server");
  }
  WireMcpHttpPost();
  McpClient client;
  if (!StartMcpClient(client, server)) {
    return Error("article feed server unavailable");
  }
  auto result = client.CallTool("blog_articles", arguments);
  client.Stop();
  if (!result) {
    return result.error();
  }
  if (result->getIf<bool>("isError").value_or(false)) {
    return Error("article feed tool failed");
  }
  return UnwrapMcpTextResult(DumpJson(*result));
}

} // namespace pbr
