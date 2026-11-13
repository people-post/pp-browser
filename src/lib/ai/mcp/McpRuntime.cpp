#include "lib/ai/mcp/McpRuntime.h"

#include "foundation/platform/Platform.h"

#include <mutex>

namespace pbr {

bool StartMcpClient(McpClient& client, const McpConfig& config, const long request_timeout_s) {
  if (!config.enabled || !config.IsConfigured()) {
    return false;
  }
  if (!config.url.empty()) {
    if (!client.StartHttp(config.url)) {
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
    if (auto initialized = client.Initialize(request_timeout_s); !initialized) {
      // HTTP is stateless per request: a failed initialize (typically the phone's network is not up yet at
      // launch) must not cost the server for the session; the next tools/list simply tries again. A stdio
      // server whose initialize failed is dropped: its process is not trustworthy (#300).
      if (config.url.empty()) {
        client.Stop(); // Initialize already logged why
        return false;
      }
    }
  }
  return client.IsRunning();
}

void McpRuntime::Stop() {
  std::lock_guard lock(mu_);
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

bool McpRuntime::Start(const AppConfig& config, const AppConfig& defaults, const long request_timeout_s) {
  std::lock_guard lock(mu_);
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

  bool all_started = true;
  const McpConfig promoted_config = ResolvePromotedMcp(config, defaults);
  if (promoted_config.IsConfigured()) {
    promoted = std::make_unique<McpClient>();
    if (!StartMcpClient(*promoted, promoted_config, request_timeout_s)) {
      promoted.reset();
      all_started = !promoted_config.enabled; // a disabled server is not a failure
    }
  }

  for (const McpConfig& entry : config.mcp_servers) {
    if (!entry.enabled || !entry.IsConfigured()) {
      continue;
    }
    auto client = std::make_unique<McpClient>();
    if (StartMcpClient(*client, entry, request_timeout_s)) {
      custom.push_back(std::move(client));
    } else {
      all_started = false;
    }
  }
  return all_started;
}

McpClient* McpRuntime::PromotedPtr() {
  std::lock_guard lock(mu_);
  return promoted && promoted->IsRunning() ? promoted.get() : nullptr;
}

std::vector<McpClient*> McpRuntime::CustomPtrs() const {
  std::lock_guard lock(mu_);
  std::vector<McpClient*> out;
  for (const std::unique_ptr<McpClient>& client : custom) {
    if (client && client->IsRunning()) {
      out.push_back(client.get());
    }
  }
  return out;
}

} // namespace pbr
