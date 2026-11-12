#pragma once

#include "lib/ai/mcp/McpClient.h"
#include "foundation/data/Config.h"

#include <memory>
#include <mutex>
#include <vector>

namespace pbr {

struct McpRuntime {
  std::unique_ptr<McpClient> promoted;
  std::vector<std::unique_ptr<McpClient>> custom;

  void Stop();
  /**
   * Starts every enabled server (`request_timeout_s`: an HTTP server's initialize bound, 0 = the transport's
   * default). False when a configured server did not start.
   */
  bool Start(const AppConfig& config, const AppConfig& defaults, long request_timeout_s = 0);
  McpClient* PromotedPtr();
  std::vector<McpClient*> CustomPtrs() const;

private:
  mutable std::mutex mu_;
};

/** `request_timeout_s`: an HTTP server's initialize bound (0 = the transport's default). */
bool StartMcpClient(McpClient& client, const McpConfig& config, long request_timeout_s = 0);

} // namespace pbr
