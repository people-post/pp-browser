#include "feature/ai/SharedMcpTools.h"

#include "lib/ai/tools/ToolRegistryBuild.h"

#include <chrono>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

SharedMcpTools::SharedMcpTools() {
  redirectLogger("AgentSession");
}

std::shared_ptr<McpConnection> SharedMcpTools::Acquire(const AppConfig& config, const std::atomic<bool>* cancel) {
  std::unique_lock lock(mu_);
  while (!kept_ && connecting_) {
    // Another turn connects: wait for it rather than open a second connection.
    if (cancel && cancel->load()) {
      return nullptr;
    }
    cv_.wait_for(lock, std::chrono::milliseconds(20));
  }
  if (kept_) {
    return kept_;
  }
  connecting_ = true;
  const uint64_t generation = generation_;
  lock.unlock();

  std::shared_ptr<McpConnection> connection = Connect(config); // network; nothing held

  lock.lock();
  connecting_ = false;
  if (connection->complete && generation == generation_) {
    kept_ = connection;
  }
  lock.unlock();
  cv_.notify_all();
  return connection;
}

void SharedMcpTools::Invalidate() {
  std::lock_guard lock(mu_);
  ++generation_;
  kept_.reset(); // sessions that attached it keep it until they attach the next one
}

std::shared_ptr<McpConnection> SharedMcpTools::Connect(const AppConfig& config) {
  const auto started = std::chrono::steady_clock::now();
  auto connection = std::make_shared<McpConnection>();
  const bool started_all =
      connection->runtime.Start(config, Config::DefaultAppConfig(), kConnectRequestTimeoutS);

  std::vector<std::string> custom_prefixes;
  custom_prefixes.reserve(config.mcp_servers.size());
  for (const McpConfig& entry : config.mcp_servers) {
    custom_prefixes.push_back(entry.id);
  }
  const bool listed_all = RegisterMcpServerTools(connection->tools, connection->runtime.PromotedPtr(),
                                                 connection->runtime.CustomPtrs(), custom_prefixes,
                                                 kConnectRequestTimeoutS);
  connection->complete = started_all && listed_all;

  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  log().info << "MCP connected in " << ms << " ms: " << connection->tools.Tools().size() << " tool(s)"
             << (connection->complete ? "" : " (incomplete: the next turn connects again)");
  for (const ToolDescriptor& tool : connection->tools.Tools()) {
    log().info << "  - " << tool.definition.name;
  }
  return connection;
}

} // namespace pbr
