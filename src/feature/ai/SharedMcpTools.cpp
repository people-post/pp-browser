#include "feature/ai/SharedMcpTools.h"

#include "lib/ai/tools/ToolRegistryBuild.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::atomic<size_t> g_running_connects{0};
std::atomic<size_t> g_waiters{0};

/** Counts a waiter for as long as it lives. */
struct WaiterCount {
  WaiterCount() { ++g_waiters; }
  ~WaiterCount() { --g_waiters; }
  WaiterCount(const WaiterCount&) = delete;
  WaiterCount& operator=(const WaiterCount&) = delete;
};

} // namespace

/** What SharedMcpTools shares with its connect threads. Guarded by `mu`. */
struct SharedMcpTools::State : public Module {
  State() { redirectLogger("AgentSession"); }
  logging::Logger& Log() const { return log(); }

  std::mutex mu;
  std::condition_variable cv;
  uint64_t generation = 0;
  bool closed = false;
  std::chrono::milliseconds cooldown{kRetryCooldown};
  std::shared_ptr<McpConnection> kept;
  std::shared_ptr<McpConnection> failed; // unreachable; handed out until failed_until
  std::chrono::steady_clock::time_point failed_until{};
  std::shared_ptr<Flight> flight; // this generation's connect in flight
};

/** One connect: its waiters keep it and read the result when `done`. Guarded by State::mu. */
struct SharedMcpTools::Flight {
  uint64_t generation = 0;
  bool done = false;
  std::shared_ptr<McpConnection> result;
};

SharedMcpTools::SharedMcpTools() : state_(std::make_shared<State>()) {}

SharedMcpTools::~SharedMcpTools() {
  std::lock_guard lock(state_->mu);
  state_->closed = true; // a connect in flight drops its result when it ends
  state_->flight.reset();
  state_->kept.reset();
  state_->failed.reset();
}

std::shared_ptr<McpConnection> SharedMcpTools::Acquire(const AppConfig& config, const std::atomic<bool>* cancel) {
  std::unique_lock lock(state_->mu);
  if (state_->kept) {
    return state_->kept;
  }
  if (state_->failed && std::chrono::steady_clock::now() < state_->failed_until) {
    return state_->failed; // offline a moment ago: no dial, no wait
  }
  std::shared_ptr<Flight> flight = state_->flight;
  if (!flight) {
    flight = std::make_shared<Flight>();
    flight->generation = state_->generation;
    state_->flight = flight;
    ++g_running_connects;
    try {
      std::thread(&SharedMcpTools::RunConnect, state_, flight, config).detach();
    } catch (const std::exception& e) {
      --g_running_connects;
      state_->Log().error << "MCP connect thread not started: " << e.what();
      state_->flight.reset();
      flight->done = true;
      flight->result = std::make_shared<McpConnection>();
    }
  }
  // Every turn that needs the connection waits here, the one that started it too: a stop ends the wait at once and
  // the connect goes on for the next turn.
  WaiterCount counted;
  while (!flight->done) {
    if (cancel && cancel->load()) {
      return nullptr;
    }
    state_->cv.wait_for(lock, std::chrono::milliseconds(20));
  }
  return flight->result;
}

void SharedMcpTools::Invalidate() {
  std::lock_guard lock(state_->mu);
  ++state_->generation;
  state_->kept.reset(); // sessions that attached it keep it until they attach the next one
  state_->failed.reset();
  state_->flight.reset(); // its waiters still get its result; it is not kept
}

void SharedMcpTools::SetRetryCooldownForTests(const std::chrono::milliseconds cooldown) {
  std::lock_guard lock(state_->mu);
  state_->cooldown = cooldown;
}

size_t SharedMcpTools::RunningConnectsForTests() {
  return g_running_connects.load();
}

size_t SharedMcpTools::WaitersForTests() {
  return g_waiters.load();
}

void SharedMcpTools::RunConnect(std::shared_ptr<State> state, std::shared_ptr<Flight> flight, AppConfig config) {
  const auto started = std::chrono::steady_clock::now();
  auto connection = std::make_shared<McpConnection>();
  try {
    const bool started_all = connection->runtime.Start(config, Config::DefaultAppConfig(), kConnectRequestTimeoutS);
    std::vector<std::string> custom_prefixes;
    custom_prefixes.reserve(config.mcp_servers.size());
    for (const McpConfig& entry : config.mcp_servers) {
      custom_prefixes.push_back(entry.id);
    }
    McpClient* promoted = connection->runtime.PromotedPtr();
    const std::vector<McpClient*> custom = connection->runtime.CustomPtrs();
    const bool listed_all =
        RegisterMcpServerTools(connection->tools, promoted, custom, custom_prefixes, kConnectRequestTimeoutS);
    connection->complete = started_all && listed_all;
    connection->unreachable = promoted && !promoted->LastRequestAnswered();
    for (McpClient* client : custom) {
      connection->unreachable = connection->unreachable || !client->LastRequestAnswered();
    }
  } catch (const std::exception& e) {
    state->Log().error << "MCP connect failed: " << e.what();
    connection = std::make_shared<McpConnection>(); // incomplete, not unreachable: the next turn connects again
  } catch (...) {
    state->Log().error << "MCP connect failed: unknown exception";
    connection = std::make_shared<McpConnection>();
  }

  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  state->Log().info << "MCP connected in " << ms << " ms: " << connection->tools.Tools().size() << " tool(s)"
                    << (connection->complete      ? ""
                        : connection->unreachable ? " (unreachable: turns go without MCP tools for a while)"
                                                  : " (incomplete: the next turn connects again)");
  for (const ToolDescriptor& tool : connection->tools.Tools()) {
    state->Log().info << "  - " << tool.definition.name;
  }

  {
    std::lock_guard lock(state->mu);
    flight->done = true;
    flight->result = connection;
    if (state->flight == flight) {
      state->flight.reset();
    }
    if (!state->closed && flight->generation == state->generation) {
      if (connection->complete) {
        state->kept = connection;
      } else if (connection->unreachable) {
        state->failed = connection;
        state->failed_until = std::chrono::steady_clock::now() + state->cooldown;
      }
    }
  }
  state->cv.notify_all();
  connection.reset();
  flight.reset();
  state.reset(); // may destroy the state (SharedMcpTools is gone): before the count says this thread is done
  --g_running_connects;
}

} // namespace pbr
