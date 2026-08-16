#include "base/p2p/Libp2pScheduler.h"

#include "base/p2p/Libp2pWorker.h"

namespace pbr {

Libp2pScheduler::Libp2pScheduler(Libp2pHost& host) : host_(host) {}

void Libp2pScheduler::Post(Libp2pExecutor executor, std::function<void()> task) {
  if (!task) {
    return;
  }
  switch (executor) {
  case Libp2pExecutor::ControlCritical:
    PostLibp2pWorker(host_, WorkerLane::Critical, std::move(task));
    break;
  case Libp2pExecutor::ControlNormal:
    PostLibp2pWorker(host_, WorkerLane::Normal, std::move(task));
    break;
  case Libp2pExecutor::ControlBackground:
    PostLibp2pWorker(host_, WorkerLane::Background, std::move(task));
    break;
  case Libp2pExecutor::HostIo:
    host_.Post(std::move(task));
    break;
  }
}

void Libp2pScheduler::PostToSession(Libp2pSessionId session_id, std::function<void()> task) {
  if (!task) {
    return;
  }
  auto strand = StrandFor(session_id);
  boost::asio::post(*strand, std::move(task));
}

Libp2pSessionId Libp2pScheduler::NextSessionId() {
  return next_session_id_.fetch_add(1, std::memory_order_relaxed);
}

void Libp2pScheduler::SetComputePool(WorkerPool* pool) {
  compute_pool_ = pool;
}

bool Libp2pScheduler::PostCompute(std::function<void()> task) {
  if (!task || compute_pool_ == nullptr) {
    return false;
  }
  compute_pool_->Post(WorkerLane::Normal, std::move(task));
  return true;
}

void Libp2pScheduler::ClearSessionStrands() {
  std::lock_guard lock(strands_mu_);
  strands_.clear();
}

boost::asio::any_io_executor Libp2pScheduler::HostIoExecutor() const {
  return host_.IoExecutor();
}

std::shared_ptr<Libp2pScheduler::Strand> Libp2pScheduler::StrandFor(Libp2pSessionId session_id) {
  {
    std::lock_guard lock(strands_mu_);
    auto it = strands_.find(session_id);
    if (it != strands_.end()) {
      return it->second;
    }
  }
  auto strand = std::make_shared<Strand>(HostIoExecutor());
  {
    std::lock_guard lock(strands_mu_);
    auto [it, inserted] = strands_.emplace(session_id, strand);
    if (!inserted) {
      return it->second;
    }
  }
  return strand;
}

} // namespace pbr
