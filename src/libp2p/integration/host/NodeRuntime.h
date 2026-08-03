#pragma once

#include "common/Error.h"
#include "common/WorkerPool.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/IdentifyIntegrationService.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Shared libp2p host + session lifecycle for pp-browser Node and headless pp-node (N011).
 * Does not pull MessagingHub / SDL / chat stacks.
 */
struct NodeRuntimeConfig {
  Libp2pHostConfig host;
  PeerSessionConfig sessions;
  std::vector<std::string> bootstrap_peers;
  /** Required — typically ThreadRuntime::Workers() from the composition root. */
  WorkerPool* worker_pool = nullptr;
  /**
   * When host.listen_enabled, try these multiaddrs in order.
   * Empty → use host.listen_multiaddr only (fail-loud single attempt).
   */
  std::vector<std::string> listen_candidates;
};

class NodeRuntime {
public:
  NodeRuntime();
  ~NodeRuntime();

  NodeRuntime(const NodeRuntime&) = delete;
  NodeRuntime& operator=(const NodeRuntime&) = delete;

  Roe<void> Start(const NodeRuntimeConfig& config);
  void Stop();

  bool IsRunning() const;
  Libp2pHost* Host();
  PeerSessionManager* Sessions() const;
  IdentifyIntegrationService* Identify();

  /** Bound listen multiaddr after a successful Node start (may differ from requested). */
  const std::string& BoundListenMultiaddr() const { return bound_listen_; }
  const std::string& LastError() const { return last_error_; }

  /** Ephemeral listen while Client (mobile call-scoped — N025). */
  bool EphemeralListenActive() const { return ephemeral_listen_active_; }
  Roe<void> StartEphemeralListen();
  /** Non-blocking N025 start; `cb` is invoked on BrowserThread IO when finished. */
  void StartEphemeralListenAsync(std::function<void(Roe<void>)> cb);
  void StopEphemeralListen();
  /** Non-blocking N025 stop; `cb` is invoked on BrowserThread IO when finished. */
  void StopEphemeralListenAsync(std::function<void()> cb);

  void Tick();
  void SuspendColdPeers();

private:
  void RegisterBootstrapPeers(const std::vector<std::string>& bootstrap_peers);

  std::unique_ptr<Libp2pHost> host_;
  std::unique_ptr<PeerSessionManager> sessions_;
  std::unique_ptr<IdentifyIntegrationService> identify_;
  std::string bound_listen_;
  std::string last_error_;
  bool ephemeral_listen_active_ = false;
};

/** Prefer a concrete TCP listen multiaddr from the host, else `requested`. */
std::string ResolveBoundListenMultiaddr(Libp2pHost& host, const std::string& requested);

/** Map AppConfig-style session knobs; mobile caps applied by caller if needed. */
PeerSessionConfig MakePeerSessionConfig(size_t max_connections, size_t max_concurrent_dials,
                                        int dial_timeout_ms, int idle_ttl_ms,
                                        int dial_failure_backoff_ms);

} // namespace pbr
