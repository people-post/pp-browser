#include "base/ai/mcp/McpClient.h"
#include "base/net/McpInfraBridge.h"

#include <cassert>
#include <iostream>

int main() {
  const nlohmann::json tool_result = {
      {"content", nlohmann::json::array({{{"type", "text"}, {"text", R"({"success":true,"relay_user_id":"relay:abc"})"}}})}};

  auto parsed = pbr::ParseMcpToolJsonResult(tool_result);
  assert(parsed);
  assert((*parsed)["success"].get<bool>());
  assert((*parsed)["relay_user_id"] == "relay:abc");

  auto& client = pbr::McpClient::MockInstance();
  auto register_result = pbr::CallMcpToolJson(client, "register_user", {{"nickname", "alice"}});
  assert(register_result);
  assert((*register_result)["relay_user_id"] == "relay:test");

  std::cout << "mcp_infra_bridge_test ok\n";
  return 0;
}
