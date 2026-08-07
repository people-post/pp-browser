#pragma once

#include "app/node/StatusHttpProtocol.h"
#include "common/Error.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace pbr {

/**
 * Tiny HTTP/1.1 admin server for pp-node ops (/healthz, /status).
 * Not peer-facing — default bind is loopback; set STATUS_ADDR / --status-addr to expose.
 */
class StatusHttpServer {
public:
  using SnapshotFn = std::function<StatusHttpSnapshot()>;

  StatusHttpServer();
  ~StatusHttpServer();

  StatusHttpServer(const StatusHttpServer&) = delete;
  StatusHttpServer& operator=(const StatusHttpServer&) = delete;

  /** Start accept loop on a background thread. Binds whatever host the operator set. */
  Roe<void> Start(const StatusHttpBind& bind, StatusHttpAuthConfig auth, SnapshotFn snapshot);

  void Stop();

  bool IsRunning() const { return running_.load(); }
  std::string BoundEndpoint() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

} // namespace pbr
