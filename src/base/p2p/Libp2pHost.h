#pragma once

#include "common/Error.h"
#include "common/WorkerPool.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include "common/PbrCompat.h"

namespace libp2p {
struct Host;
namespace crypto {
struct KeyPair;
}
namespace peer {
class PeerId;
}
namespace multi {
class Multiaddress;
}
} // namespace libp2p

namespace pbr {

struct Libp2pHostConfig {
  std::string listen_multiaddr = "/ip4/0.0.0.0/tcp/18517";
  /** When false, start host for outbound dials only (Client role). */
  bool listen_enabled = true;
  /** Optional app device ML-DSA-65 identity (4032-byte private + 1952-byte public). When unset, host generates one. */
  std::optional<std::vector<uint8_t>> device_ml_dsa_private_key;
  std::optional<std::vector<uint8_t>> device_ml_dsa_public_key;
};

/** Shared libp2p Host (Yamux + Noise over TCP). Owned by MessagingHub. */
class Libp2pHost {
public:
  Libp2pHost();
  ~Libp2pHost();

  Libp2pHost(const Libp2pHost&) = delete;
  Libp2pHost& operator=(const Libp2pHost&) = delete;

  Roe<void> Start(const Libp2pHostConfig& config = {});
  void Stop();

  bool IsAvailable() const { return available_; }
  bool IsRunning() const { return running_.load(); }

  libp2p::Host& GetHost();
  const libp2p::Host& GetHost() const;
  std::shared_ptr<libp2p::Host> SharedHost() const;

  /** Local PeerId base58 once started. */
  Roe<std::string> LocalPeerIdBase58() const;

  /** Listen multiaddrs (may be empty before listen completes). */
  std::vector<std::string> ListenMultiaddrs() const;

  /** Dispatch work onto the host io_context thread. */
  void Post(std::function<void()> fn);

  /** Executor for host io_context (async pumps, strands). */
  boost::asio::any_io_executor IoExecutor() const;

  /** Bounded worker pool for blocking protocol / HTTP hop-offs (not the io thread). */
  WorkerPool& GetWorkerPool();
  const WorkerPool& GetWorkerPool() const;

  /** Block until fn completes on the io thread (or return error if not running). */
  Roe<void> PostAndWait(std::function<void()> fn);

  /** Add a listen address on a running host (ephemeral mobile listen — N025). Blocks. */
  Roe<void> ListenOn(const std::string& multiaddr);

  /**
   * Same as ListenOn but never blocks the caller: work runs on the libp2p io thread,
   * then `cb` is invoked on that same thread. Prefer this from worker pool so
   * AcceptInvite / chat are not stuck behind a hung bind (Samsung N025 dogfood).
   */
  void ListenOnAsync(const std::string& multiaddr, std::function<void(Roe<void>)> cb);

  /** Close and remove all listeners; host keeps running for outbound dials. Blocks. */
  Roe<void> StopListening();

  /** Non-blocking StopListening; `cb` runs on the libp2p io thread when done. */
  void StopListeningAsync(std::function<void()> cb);

private:
  void EnsureLogging();
  Roe<void> ListenOnIoThread(const libp2p::multi::Multiaddress& ma, const std::string& addr);
  void StopListeningIoThread();

  bool available_ = false;
  std::atomic<bool> running_{false};
  std::shared_ptr<boost::asio::io_context> io_context_;
  /** Keeps io_context::run() alive when the host has no pending handlers (Client / idle). */
  std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
  std::shared_ptr<libp2p::Host> host_;
  std::unique_ptr<WorkerPool> owned_worker_pool_;
  WorkerPool* worker_pool_ = nullptr;
  std::thread io_thread_;
  mutable std::mutex mutex_;
  Libp2pHostConfig config_;
};

} // namespace pbr
