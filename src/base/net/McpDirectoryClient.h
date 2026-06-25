#pragma once

#include "base/ai/mcp/McpClient.h"
#include "base/net/ServiceClients.h"

namespace pbr {

class McpDirectoryClient : public IDirectoryClient {
public:
  explicit McpDirectoryClient(McpClient* client = nullptr);

  void SetClient(McpClient* client);
  Roe<std::vector<DirectoryHit>> SearchPeople(const std::string& query) override;

private:
  McpClient* client_ = nullptr;
};

} // namespace pbr
