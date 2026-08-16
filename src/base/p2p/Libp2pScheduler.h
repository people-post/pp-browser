#pragma once

#include "common/WorkerPool.h"
#include "base/p2p/Libp2pHost.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace pbr {

enum class Libp2pExecutor {
  ControlCritical,
  ControlNormal,
  ControlBackground,
  HostIo,
};

using Libp2pSessionId = uint64_t;

/**
 * Dispatch facade for libp2p integration executor classes.
 * Control* → app WorkerPool (via PostLibp2pWorker); HostIo → Libp2pHost io_context.
 */
class Libp2pScheduler {
public:
  explicit Libp2pScheduler(Libp2pHost& host);

  Libp2pScheduler(const Libp2pScheduler&) = delete;
  Libp2pScheduler& operator=(const Libp2pScheduler&) = delete;

  void Post(Libp2pExecutor executor, std::function<void()> task);
  void PostToSession(Libp2pSessionId session_id, std::function<void()> task);

  Libp2pSessionId NextSessionId();

  /** Optional headless compute pool (blockchain relay, etc.). Returns false when unset. */
  void SetComputePool(WorkerPool* pool);
  bool PostCompute(std::function<void()> task);

  void ClearSessionStrands();

private:
  using Strand = boost::asio::strand<boost::asio::any_io_executor>;

  boost::asio::any_io_executor HostIoExecutor() const;
  std::shared_ptr<Strand> StrandFor(Libp2pSessionId session_id);

  Libp2pHost& host_;
  std::atomic<Libp2pSessionId> next_session_id_{1};
  WorkerPool* compute_pool_ = nullptr;
  mutable std::mutex strands_mu_;
  std::unordered_map<Libp2pSessionId, std::shared_ptr<Strand>> strands_;
};

} // namespace pbr
