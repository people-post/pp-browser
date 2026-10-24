#include "lib/ai/mcp/McpClient.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// Whether a failed tools/list still reached the server: an HTTP status (any) or a JSON-RPC error is an answer
// (waiting for it to list more cannot help); no connection is not.
class McpHttpAnswerTest : public ::testing::Test {
protected:
  void TearDown() override { McpClient::SetHttpPost(nullptr); }

  void Serve(Roe<HttpResponse> response) {
    McpClient::SetHttpPost([response](const std::string&, const std::string&,
                                      const std::map<std::string, std::string>&, long) { return response; });
    ASSERT_TRUE(client_.StartHttp("http://127.0.0.1:1/mcp"));
  }

  McpClient client_;
};

TEST_F(McpHttpAnswerTest, ErrorStatusIsAnAnswer) {
  Serve(HttpResponse{.status_code = 503, .body = "Service Unavailable"});
  EXPECT_FALSE(static_cast<bool>(client_.ListTools(3)));
  EXPECT_TRUE(client_.LastRequestAnswered());
}

TEST_F(McpHttpAnswerTest, JsonRpcErrorIsAnAnswer) {
  Serve(HttpResponse{.status_code = 200,
                     .body = R"({"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"no such method"}})"});
  EXPECT_FALSE(static_cast<bool>(client_.ListTools(3)));
  EXPECT_TRUE(client_.LastRequestAnswered());
}

TEST_F(McpHttpAnswerTest, NoConnectionIsNotAnAnswer) {
  Serve(Error("connection refused"));
  EXPECT_FALSE(static_cast<bool>(client_.ListTools(3)));
  EXPECT_FALSE(client_.LastRequestAnswered());
}

TEST_F(McpHttpAnswerTest, ListingIsAnAnswer) {
  Serve(HttpResponse{.status_code = 200, .body = R"({"jsonrpc":"2.0","id":1,"result":{"tools":[]}})"});
  EXPECT_TRUE(static_cast<bool>(client_.ListTools(3)));
  EXPECT_TRUE(client_.LastRequestAnswered());
}

} // namespace
} // namespace pbr
