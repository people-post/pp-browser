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
 * Tiny loopback HTTP/1.1 admin server for pp-node ops (/healthz, /status).
 * Not peer-facing — bind loopback by default; non-loopback requires explicit allow.
 */
class StatusHttpServer {
public:
  using SnapshotFn = std::function<StatusHttpSnapshot()>;

  StatusHttpServer();
  ~StatusHttpServer();

  StatusHttpServer(const StatusHttpServer&) = delete;
  StatusHttpServer& operator=(const StatusHttpServer&) = delete;

  /**
   * Start accept loop on a background thread.
   * Fails if bind host is not loopback unless `allow_non_loopback`.
   */
  Roe<void> Start(const StatusHttpBind& bind, StatusHttpAuthConfig auth, SnapshotFn snapshot,
                  bool allow_non_loopback = false);

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
