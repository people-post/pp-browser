#pragma once

#include "common/Module.h"
#include "foundation/data/Config.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/mcp/McpRuntime.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>

namespace pbr {

/**
 * The MCP servers' connection (initialize + tools/list) and the tools they listed. Made on SharedMcpTools' own
 * thread, then shared read-only by every session that attaches it; it lives as long as one of them (or
 * SharedMcpTools) holds it. The tools' `execute` calls into `runtime`'s clients, which take requests from several
 * threads (McpClient).
 */
struct McpConnection {
  McpRuntime runtime;
  /** The MCP tools only (no web_search, no app tools), in registration order. */
  ToolRegistry tools;
  /** Every configured server started and listed at least one tool: kept for the next turns. */
  bool complete = false;
  /** A server did not answer at all (offline, a timeout): no turn dials again for kRetryCooldown. */
  bool unreachable = false;
};

/**
 * One MCP connection for the sessions of one AgentSessions (B210): no session connects while the app starts, unlocks
 * or opens a chat; the first turn that needs MCP tools starts the connect, on a thread of its own, and every turn
 * that needs it (that one included) waits for it in a wait that a stop ends at once; the connect then finishes in
 * the background and its result serves the next turn. A complete connection is kept; an unreachable one is handed
 * out for kRetryCooldown without dialing again (offline: no turn waits for a connect each time); any other
 * incomplete one is not kept, so the next turn connects again. Invalidate() (the configuration changed) drops the
 * kept and the failed connection, and a connect in flight is not kept.
 *
 * Thread-safe. Acquire blocks (turn threads only); Invalidate never waits for a connect. The connect thread holds only
 * what it shares with this object (never this object), so destroying it never waits for the network: a connect still
 * in flight finishes on its own (each request bounded by kConnectRequestTimeoutS) and its result is dropped.
 */
class SharedMcpTools {
public:
  /** Each initialize and tools/list request of a connect: a turn waits on it, so not the transport's 30 s. */
  static constexpr long kConnectRequestTimeoutS = 10;
  /** After an unreachable connect, how long turns go without MCP tools instead of dialing again. */
  static constexpr std::chrono::seconds kRetryCooldown{45};

  SharedMcpTools();
  ~SharedMcpTools();

  SharedMcpTools(const SharedMcpTools&) = delete;
  SharedMcpTools& operator=(const SharedMcpTools&) = delete;

  /**
   * The connection for `config`: the kept (or cooling-down failed) one, else the result of the connect in flight, or
   * of one started now. nullptr when `cancel` was set first (the connect goes on). Otherwise never nullptr; an
   * incomplete connection is returned too (its tools are what could be listed).
   */
  std::shared_ptr<McpConnection> Acquire(const AppConfig& config, const std::atomic<bool>* cancel = nullptr);
  /** The configuration changed: the kept connection, the cooldown and a connect in flight are dropped. */
  void Invalidate();

  /** Tests: a shorter cooldown. */
  void SetRetryCooldownForTests(std::chrono::milliseconds cooldown);
  /** Tests: connect threads still running, in the process. */
  static size_t RunningConnectsForTests();
  /** Tests: turns waiting in Acquire for a connect, in the process. */
  static size_t WaitersForTests();

private:
  struct State;
  struct Flight;
  static void RunConnect(std::shared_ptr<State> state, std::shared_ptr<Flight> flight, AppConfig config);

  std::shared_ptr<State> state_;
};

} // namespace pbr
