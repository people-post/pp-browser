#pragma once

#include "domain/ai/mcp/McpClient.h"
#include "foundation/data/Config.h"

#include <memory>
#include <mutex>
#include <vector>

namespace pbr {

struct McpRuntime {
  std::unique_ptr<McpClient> promoted;
  std::vector<std::unique_ptr<McpClient>> custom;

  void Stop();
  void Start(const AppConfig& config, const AppConfig& defaults);
  McpClient* PromotedPtr();
  std::vector<McpClient*> CustomPtrs() const;

private:
  mutable std::mutex mu_;
};

bool StartMcpClient(McpClient& client, const McpConfig& config);

} // namespace pbr
