#include "mcp/McpClient.h"
#include "mcp/SchemaAdapter.h"

#include <cassert>
#include <iostream>

int main() {
  auto& client = ppbrowser::McpClient::MockInstance();
  assert(client.IsRunning());
  assert(client.Initialize());

  auto tools = client.ListTools();
  assert(!tools.empty());
  assert(tools[0].name == "user_search");

  auto result = client.CallTool("user_search", {{"query", "ada"}});
  auto rows = ppbrowser::SchemaAdapter::ToolResultToRows(result);
  assert(rows.is_array());
  assert(!rows.empty());

  std::cout << "mcp_mock_test ok\n";
  return 0;
}
