#include "base/ai/mcp/McpClient.h"
#include "base/ai/mcp/SchemaAdapter.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

TEST(McpMockTest, InitializesAndCallsMockTools) {
  auto& client = pbr::McpClient::MockInstance();
  EXPECT_TRUE(client.IsRunning());

  auto init_result = client.Initialize();
  EXPECT_TRUE(static_cast<bool>(init_result));

  auto tools_result = client.ListTools();
  ASSERT_TRUE(static_cast<bool>(tools_result));
  ASSERT_FALSE(tools_result->empty());
  EXPECT_EQ(tools_result->front().name, "user_search");

  pbr::Object args;
  args.set("query", "ada");
  auto call_result = client.CallTool("user_search", args);
  ASSERT_TRUE(static_cast<bool>(call_result));

  auto rows_result = pbr::SchemaAdapter::ToolResultToRows(*call_result);
  ASSERT_TRUE(static_cast<bool>(rows_result));
  EXPECT_TRUE(pbr::isArrayValue(*rows_result));
  const pbr::Array* rows = pbr::asArray(*rows_result);
  ASSERT_NE(rows, nullptr);
  EXPECT_FALSE(rows->elements.empty());
}
