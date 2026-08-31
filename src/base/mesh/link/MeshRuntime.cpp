#include "base/mesh/link/MeshRuntime.h"

namespace pbr::amp {

MeshRuntime::MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
                         PeerLinkConfig config)
    : endpoint_(endpoint), links_(endpoint, std::move(local_identity), std::move(local_peer_id), std::move(config)),
      pump_(endpoint, links_) {}

void MeshRuntime::Start() { started_ = true; }

void MeshRuntime::Stop() {
  started_ = false;
  io_queue_.clear();
  io_tick_ = {};
}

void MeshRuntime::Pump() {
  pumping_ = true;
  if (io_tick_) {
    io_tick_();
  }
  for (size_t budget = 0; budget < 32 && !io_queue_.empty(); ++budget) {
    auto task = std::move(io_queue_.front());
    io_queue_.pop_front();
    if (task) {
      task();
    }
  }
  pump_.Pump();
  pumping_ = false;
}

void MeshRuntime::Tick() { pump_.Tick(); }

void MeshRuntime::PostToIo(IoTask task) {
  if (!task) {
    return;
  }
  io_queue_.push_back(std::move(task));
}

} // namespace pbr::amp
