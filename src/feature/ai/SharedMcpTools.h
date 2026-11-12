#pragma once

#include "common/Module.h"
#include "foundation/data/Config.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/mcp/McpRuntime.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace pbr {

/**
 * The MCP servers' connection (initialize + tools/list) and the tools they listed. Made on a turn's thread the first
 * time a turn needs MCP tools, then shared read-only by every session that attaches it; it lives as long as one of
 * them (or SharedMcpTools) holds it. The tools' `execute` calls into `runtime`'s clients, which take requests from
 * several threads (McpClient).
 */
struct McpConnection {
  McpRuntime runtime;
  /** The MCP tools only (no web_search, no app tools), in registration order. */
  ToolRegistry tools;
  /** Every configured server started and listed at least one tool: kept for the next turns. */
  bool complete = false;
};

/**
 * One MCP connection for the sessions of one AgentSessions (B210): no session connects while the app starts, unlocks
 * or opens a chat; the first turn that needs MCP tools connects, concurrent first turns wait for that one connection,
 * and later turns reuse it. An incomplete connection (a server did not start or listed nothing) is not kept, so the
 * next turn tries again. Invalidate() (the configuration changed) drops it: the next turn connects with the new one.
 * Thread-safe; Acquire blocks (turn threads only), Invalidate never waits for a connect.
 */
class SharedMcpTools : public Module {
public:
  SharedMcpTools();

  /** Each initialize and tools/list request of a connect: a turn waits on it, so not the transport's 30 s. */
  static constexpr long kConnectRequestTimeoutS = 10;

  /**
   * The connection for `config`, connecting when none is kept. Waiting for another thread's connect gives up when
   * `cancel` is set (nullptr then). Never nullptr otherwise; an incomplete connection is returned too (its tools are
   * what could be listed).
   */
  std::shared_ptr<McpConnection> Acquire(const AppConfig& config, const std::atomic<bool>* cancel = nullptr);
  /** The configuration changed: the kept connection is dropped and a connect in flight is not kept. */
  void Invalidate();

private:
  std::shared_ptr<McpConnection> Connect(const AppConfig& config);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  uint64_t generation_ = 0;
  bool connecting_ = false;
  std::shared_ptr<McpConnection> kept_;
};

} // namespace pbr
