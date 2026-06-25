#include "base/ai/mcp/McpRuntime.h"

#include "base/platform/Platform.h"

namespace pbr {

bool StartMcpClient(McpClient& client, const McpConfig& config) {
  if (!config.enabled || !config.IsConfigured()) {
    return false;
  }
  if (!config.url.empty()) {
    if (!client.StartHttp(config.url)) {
      return false;
    }
  } else if (config.command == "mock") {
    if (!client.Start("mock")) {
      return false;
    }
  } else if (Platform::SupportsSubprocessMcp()) {
    if (!client.Start(config.command, config.args)) {
      return false;
    }
  } else {
    return false;
  }

  if (client.IsRunning()) {
    (void)client.Initialize();
  }
  return client.IsRunning();
}

void McpRuntime::Stop() {
  if (promoted) {
    promoted->Stop();
    promoted.reset();
  }
  for (std::unique_ptr<McpClient>& client : custom) {
    if (client) {
      client->Stop();
    }
  }
  custom.clear();
}

void McpRuntime::Start(const AppConfig& config, const AppConfig& defaults) {
  Stop();

  const McpConfig promoted_config = ResolvePromotedMcp(config, defaults);
  if (promoted_config.IsConfigured()) {
    promoted = std::make_unique<McpClient>();
    if (!StartMcpClient(*promoted, promoted_config)) {
      promoted.reset();
    }
  }

  for (const McpConfig& entry : config.mcp_servers) {
    if (!entry.enabled || !entry.IsConfigured()) {
      continue;
    }
    auto client = std::make_unique<McpClient>();
    if (StartMcpClient(*client, entry)) {
      custom.push_back(std::move(client));
    }
  }
}

McpClient* McpRuntime::PromotedPtr() {
  return promoted && promoted->IsRunning() ? promoted.get() : nullptr;
}

std::vector<McpClient*> McpRuntime::CustomPtrs() const {
  std::vector<McpClient*> out;
  for (const std::unique_ptr<McpClient>& client : custom) {
    if (client && client->IsRunning()) {
      out.push_back(client.get());
    }
  }
  return out;
}

} // namespace pbr
