#pragma once

#include "common/Error.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

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
  PeerSessionManager* Sessions();

  /** Bound listen multiaddr after a successful Node start (may differ from requested). */
  const std::string& BoundListenMultiaddr() const { return bound_listen_; }
  const std::string& LastError() const { return last_error_; }

  void Tick();
  void SuspendColdPeers();

private:
  void RegisterBootstrapPeers(const std::vector<std::string>& bootstrap_peers);

  std::unique_ptr<Libp2pHost> host_;
  std::unique_ptr<PeerSessionManager> sessions_;
  std::string bound_listen_;
  std::string last_error_;
};

/** Prefer a concrete TCP listen multiaddr from the host, else `requested`. */
std::string ResolveBoundListenMultiaddr(Libp2pHost& host, const std::string& requested);

/** Map AppConfig-style session knobs; mobile caps applied by caller if needed. */
PeerSessionConfig MakePeerSessionConfig(size_t max_connections, size_t max_concurrent_dials,
                                        int dial_timeout_ms, int idle_ttl_ms,
                                        int dial_failure_backoff_ms);

} // namespace pbr
