#include "base/ai/mcp/McpClient.h"
#include "base/net/McpInfraBridge.h"

#include <gtest/gtest.h>

TEST(McpInfraBridgeTest, ParsesToolJsonAndCallsMockRegister) {
  const nlohmann::json tool_result = {
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", R"({"success":true,"relay_user_id":"relay:abc"})"}}})}};

  auto parsed = pbr::ParseMcpToolJsonResult(tool_result);
  ASSERT_TRUE(static_cast<bool>(parsed));
  EXPECT_TRUE((*parsed)["success"].get<bool>());
  EXPECT_EQ((*parsed)["relay_user_id"], "relay:abc");

  auto& client = pbr::McpClient::MockInstance();
  auto register_result = pbr::CallMcpToolJson(client, "register_user", {{"nickname", "alice"}});
  ASSERT_TRUE(static_cast<bool>(register_result));
  EXPECT_EQ((*register_result)["relay_user_id"], "relay:test");
}
