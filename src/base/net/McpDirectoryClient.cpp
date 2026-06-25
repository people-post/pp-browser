#include "base/net/McpDirectoryClient.h"

#include "base/messaging/MessagingJson.h"
#include "base/net/McpInfraBridge.h"

namespace pbr {

McpDirectoryClient::McpDirectoryClient(McpClient* client) : client_(client) {}

void McpDirectoryClient::SetClient(McpClient* client) {
  client_ = client;
}

Roe<std::vector<DirectoryHit>> McpDirectoryClient::SearchPeople(const std::string& query) {
  if (!client_) {
    return Error("MCP client not available");
  }
  auto result = CallMcpToolJson(*client_, "search_people", {{"query", query}});
  if (!result) {
    return result.error();
  }

  std::vector<DirectoryHit> hits;
  const nlohmann::json* items = nullptr;
  if (result->is_array()) {
    items = &*result;
  } else if (result->contains("hits") && (*result)["hits"].is_array()) {
    items = &(*result)["hits"];
  } else {
    return Error("Invalid search_people MCP result");
  }

  for (const auto& item : *items) {
    hits.push_back(DirectoryHitFromJson(item));
  }
  return hits;
}

} // namespace pbr
