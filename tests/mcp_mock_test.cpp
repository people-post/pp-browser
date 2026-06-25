#include "base/ai/mcp/McpClient.h"
#include "base/ai/mcp/SchemaAdapter.h"

#include <cassert>
#include <iostream>

int main() {
  auto& client = pbr::McpClient::MockInstance();
  assert(client.IsRunning());

  auto init_result = client.Initialize();
  assert(init_result);

  auto tools_result = client.ListTools();
  assert(tools_result);
  assert(!tools_result->empty());
  assert(tools_result->front().name == "user_search");

  auto call_result = client.CallTool("user_search", {{"query", "ada"}});
  assert(call_result);

  auto rows_result = pbr::SchemaAdapter::ToolResultToRows(*call_result);
  assert(rows_result);
  assert(rows_result->is_array());
  assert(!rows_result->empty());

  std::cout << "mcp_mock_test ok\n";
  return 0;
}
